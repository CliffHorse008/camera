#include "rtsp_h264.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int starts_with(const uint8_t *data, size_t size, size_t i, size_t *prefix_len) {
    if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        *prefix_len = 4;
        return 1;
    }
    if (i + 3 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
        *prefix_len = 3;
        return 1;
    }
    return 0;
}

static int base64_encode(const uint8_t *data, size_t size, char *out, size_t out_size) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t j = 0;

    while (i < size) {
        size_t remain = size - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = remain > 1 ? data[i++] : 0;
        uint32_t octet_c = remain > 2 ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        if (j + 4 >= out_size) {
            return -1;
        }
        out[j++] = tbl[(triple >> 18) & 0x3f];
        out[j++] = tbl[(triple >> 12) & 0x3f];
        out[j++] = remain > 1 ? tbl[(triple >> 6) & 0x3f] : '=';
        out[j++] = remain > 2 ? tbl[triple & 0x3f] : '=';
    }

    out[j] = '\0';
    return 0;
}

static void cleanup_owned_media(struct rtsp_h264_stream_source *stream) {
    free(stream->owned_data);
    free(stream->frames);
    free(stream->audio_data);
    free(stream->audio_frames);
    stream->owned_data = NULL;
    stream->frames = NULL;
    stream->audio_data = NULL;
    stream->audio_frames = NULL;
    stream->frame_count = 0;
    stream->audio_size = 0;
    stream->audio_frame_count = 0;
}

static int parse_frames_from_buffer(const uint8_t *data, size_t size, struct rtsp_h264_frame **frames_out,
                                    size_t *frame_count_out) {
    struct rtsp_h264_frame *frames = NULL;
    size_t frames_cap = 0;
    size_t frame_count = 0;
    size_t i = 0;
    size_t current_start = (size_t)-1;
    size_t current_end = 0;

    while (i + 3 < size) {
        size_t prefix = 0;
        size_t nal_start = 0;
        size_t nal_end = 0;
        uint8_t nal_type = 0;

        if (!starts_with(data, size, i, &prefix)) {
            ++i;
            continue;
        }

        nal_start = i + prefix;
        nal_end = nal_start;
        while (nal_end + 3 < size) {
            size_t next_prefix = 0;
            if (starts_with(data, size, nal_end, &next_prefix)) {
                break;
            }
            ++nal_end;
        }
        if (nal_end + 3 >= size) {
            nal_end = size;
        }

        nal_type = (uint8_t)(data[nal_start] & 0x1f);
        if (nal_type == 9) {
            if (current_start != (size_t)-1 && current_end > current_start) {
                if (frame_count == frames_cap) {
                    size_t new_cap = frames_cap == 0 ? 64 : frames_cap * 2;
                    struct rtsp_h264_frame *new_frames =
                        (struct rtsp_h264_frame *)realloc(frames, new_cap * sizeof(*new_frames));
                    if (new_frames == NULL) {
                        free(frames);
                        return -1;
                    }
                    frames = new_frames;
                    frames_cap = new_cap;
                }
                frames[frame_count].data = data + current_start;
                frames[frame_count].size = current_end - current_start;
                ++frame_count;
            }
            current_start = (size_t)-1;
            current_end = 0;
        } else {
            if (current_start == (size_t)-1) {
                current_start = i;
            }
            current_end = nal_end;
        }
        i = nal_end;
    }

    if (current_start != (size_t)-1 && current_end > current_start) {
        if (frame_count == frames_cap) {
            size_t new_cap = frames_cap == 0 ? 1 : frames_cap * 2;
            struct rtsp_h264_frame *new_frames =
                (struct rtsp_h264_frame *)realloc(frames, new_cap * sizeof(*new_frames));
            if (new_frames == NULL) {
                free(frames);
                return -1;
            }
            frames = new_frames;
        }
        frames[frame_count].data = data + current_start;
        frames[frame_count].size = current_end - current_start;
        ++frame_count;
    }

    if (frame_count == 0) {
        free(frames);
        return -1;
    }

    *frames_out = frames;
    *frame_count_out = frame_count;
    return 0;
}

static int configure_stream_source(struct rtsp_h264_stream_source *stream, const uint8_t *data, size_t size,
                                   const char *name) {
    struct rtsp_h264_nal_unit nals[64];
    struct rtsp_h264_frame *frames = NULL;
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_size = 0;
    size_t pps_size = 0;
    char sps_b64[128];
    char pps_b64[64];
    size_t nal_count = 0;

    memset(stream, 0, sizeof(*stream));

    /* 先提取 SPS/PPS，供 DESCRIBE 返回合法的 H264 fmtp 参数。 */
    nal_count = rtsp_h264_parse_nals_from_buffer(data, size, nals, 64);
    if (nal_count == 0) {
        return -1;
    }

    for (size_t i = 0; i < nal_count; ++i) {
        uint8_t nal_type = (uint8_t)(nals[i].data[0] & 0x1f);
        if (nal_type == 7 && sps == NULL) {
            sps = nals[i].data;
            sps_size = nals[i].size;
        } else if (nal_type == 8 && pps == NULL) {
            pps = nals[i].data;
            pps_size = nals[i].size;
        }
    }

    if (sps == NULL || pps == NULL || sps_size < 4) {
        return -1;
    }
    if (base64_encode(sps, sps_size, sps_b64, sizeof(sps_b64)) < 0 ||
        base64_encode(pps, pps_size, pps_b64, sizeof(pps_b64)) < 0) {
        return -1;
    }
    if (parse_frames_from_buffer(data, size, &frames, &stream->frame_count) < 0) {
        return -1;
    }

    stream->data = data;
    stream->size = size;
    stream->frames = frames;
    stream->video_fps_num = 25;
    stream->video_fps_den = 1;
    snprintf(stream->sprop_parameter_sets, sizeof(stream->sprop_parameter_sets), "%s,%s", sps_b64,
             pps_b64);
    snprintf(stream->profile_level_id, sizeof(stream->profile_level_id), "%02X%02X%02X", sps[1], sps[2],
             sps[3]);
    snprintf(stream->stream_name, sizeof(stream->stream_name), "%s", name);
    return 0;
}

static int read_file_into_buffer(const char *path, uint8_t **buf_out, size_t *size_out) {
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    long file_size = 0;

    *buf_out = NULL;
    *size_out = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    if (file_size == 0) {
        fclose(fp);
        return 0;
    }

    buf = (uint8_t *)malloc((size_t)file_size);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *buf_out = buf;
    *size_out = (size_t)file_size;
    return 0;
}

static int parse_adts_frames(struct rtsp_h264_stream_source *stream) {
    static const uint32_t sample_rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                            24000, 22050, 16000, 12000, 11025, 8000,
                                            7350};
    struct rtsp_aac_frame *frames = NULL;
    size_t frames_cap = 0;
    size_t frame_count = 0;
    size_t offset = 0;
    uint8_t profile = 0;
    uint8_t sample_rate_index = 0;
    uint8_t channel_config = 0;

    while (offset + 7 <= stream->audio_size) {
        size_t header_size = 0;
        size_t frame_size = 0;

        if (stream->audio_data[offset] != 0xff || (stream->audio_data[offset + 1] & 0xf0) != 0xf0) {
            free(frames);
            return -1;
        }

        profile = (uint8_t)((stream->audio_data[offset + 2] >> 6) & 0x03);
        sample_rate_index = (uint8_t)((stream->audio_data[offset + 2] >> 2) & 0x0f);
        channel_config = (uint8_t)(((stream->audio_data[offset + 2] & 0x01) << 2) |
                                   ((stream->audio_data[offset + 3] >> 6) & 0x03));
        header_size = (stream->audio_data[offset + 1] & 0x01) ? 7u : 9u;
        frame_size = (size_t)(((stream->audio_data[offset + 3] & 0x03) << 11) |
                              ((uint16_t)stream->audio_data[offset + 4] << 3) |
                              ((stream->audio_data[offset + 5] >> 5) & 0x07));

        if (sample_rate_index >= sizeof(sample_rates) / sizeof(sample_rates[0]) ||
            channel_config == 0 || frame_size < header_size || offset + frame_size > stream->audio_size) {
            free(frames);
            return -1;
        }

        if (frame_count == frames_cap) {
            size_t new_cap = frames_cap == 0 ? 64 : frames_cap * 2;
            struct rtsp_aac_frame *new_frames =
                (struct rtsp_aac_frame *)realloc(frames, new_cap * sizeof(*new_frames));
            if (new_frames == NULL) {
                free(frames);
                return -1;
            }
            frames = new_frames;
            frames_cap = new_cap;
        }

        frames[frame_count].data = stream->audio_data + offset + header_size;
        frames[frame_count].size = frame_size - header_size;
        ++frame_count;
        offset += frame_size;
    }

    if (frame_count == 0 || offset != stream->audio_size) {
        free(frames);
        return -1;
    }

    stream->audio_frames = frames;
    stream->audio_frame_count = frame_count;
    stream->audio_sample_rate = sample_rates[sample_rate_index];
    stream->audio_samples_per_frame = 1024;
    stream->audio_channels = channel_config;

    {
        uint8_t audio_object_type = (uint8_t)(profile + 1);
        uint16_t asc = (uint16_t)((audio_object_type << 11) | (sample_rate_index << 7) |
                                  (channel_config << 3));
        snprintf(stream->audio_config_hex, sizeof(stream->audio_config_hex), "%04X", asc);
    }

    return 0;
}

static int run_child_to_null(const char *const argv[]) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int capture_child_stdout(const char *const argv[], char *out, size_t out_size) {
    int pipefd[2];
    pid_t pid;
    ssize_t nread = 0;
    size_t total = 0;
    int status = 0;

    if (pipe(pipefd) < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        if (devnull >= 0) {
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);
    while ((nread = read(pipefd[0], out + total, out_size > total ? out_size - total - 1 : 0)) > 0) {
        total += (size_t)nread;
        if (total + 1 >= out_size) {
            break;
        }
    }
    close(pipefd[0]);
    out[total] = '\0';

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int parse_fraction(const char *value, uint32_t *num_out, uint32_t *den_out) {
    unsigned int num = 0;
    unsigned int den = 0;

    if (sscanf(value, "%u/%u", &num, &den) != 2 || num == 0 || den == 0) {
        return -1;
    }
    *num_out = (uint32_t)num;
    *den_out = (uint32_t)den;
    return 0;
}

static int probe_mp4_video_info(const char *path, uint32_t *fps_num_out, uint32_t *fps_den_out) {
    const char *const argv[] = {"ffprobe",
                                "-v",
                                "error",
                                "-select_streams",
                                "v:0",
                                "-show_entries",
                                "stream=codec_name,avg_frame_rate",
                                "-of",
                                "default=noprint_wrappers=1:nokey=0",
                                path,
                                NULL};
    char output[1024];
    char *codec_line = NULL;
    char *fps_line = NULL;

    if (capture_child_stdout(argv, output, sizeof(output)) < 0) {
        return -1;
    }

    codec_line = strstr(output, "codec_name=");
    fps_line = strstr(output, "avg_frame_rate=");
    if (codec_line == NULL || fps_line == NULL) {
        return -1;
    }
    codec_line += strlen("codec_name=");
    if (strncmp(codec_line, "h264", 4) != 0) {
        return -1;
    }
    fps_line += strlen("avg_frame_rate=");
    if (parse_fraction(fps_line, fps_num_out, fps_den_out) < 0) {
        return -1;
    }
    return 0;
}

static int make_temp_path(char *path, size_t path_size, const char *tag) {
    int fd;

    snprintf(path, path_size, "/tmp/rtsp_%s_XXXXXX", tag);
    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    close(fd);
    return 0;
}

int rtsp_h264_stream_source_load_mp4(struct rtsp_h264_stream_source *stream, const char *path) {
    char video_tmp[64] = {0};
    char audio_tmp[64] = {0};
    uint8_t *video_buf = NULL;
    uint8_t *audio_buf = NULL;
    size_t video_size = 0;
    size_t audio_size = 0;
    uint32_t fps_num = 25;
    uint32_t fps_den = 1;
    const char *const ffmpeg_video_argv[] = {"ffmpeg",
                                             "-y",
                                             "-i",
                                             path,
                                             "-map",
                                             "0:v:0",
                                             "-an",
                                             "-c:v",
                                             "libx264",
                                             "-preset",
                                             "ultrafast",
                                             "-tune",
                                             "zerolatency",
                                             "-x264-params",
                                             "aud=1:repeat-headers=1",
                                             "-f",
                                             "h264",
                                             video_tmp,
                                             NULL};
    const char *const ffmpeg_audio_argv[] = {"ffmpeg",
                                             "-y",
                                             "-i",
                                             path,
                                             "-map",
                                             "0:a:0?",
                                             "-vn",
                                             "-c:a",
                                             "aac",
                                             "-profile:a",
                                             "aac_low",
                                             "-f",
                                             "adts",
                                             audio_tmp,
                                             NULL};

    memset(stream, 0, sizeof(*stream));

    if (probe_mp4_video_info(path, &fps_num, &fps_den) < 0) {
        return -1;
    }
    if (make_temp_path(video_tmp, sizeof(video_tmp), "video") < 0 ||
        make_temp_path(audio_tmp, sizeof(audio_tmp), "audio") < 0) {
        if (video_tmp[0] != '\0') {
            unlink(video_tmp);
        }
        if (audio_tmp[0] != '\0') {
            unlink(audio_tmp);
        }
        return -1;
    }

    if (run_child_to_null(ffmpeg_video_argv) < 0) {
        unlink(video_tmp);
        unlink(audio_tmp);
        return -1;
    }
    (void)run_child_to_null(ffmpeg_audio_argv);

    if (read_file_into_buffer(video_tmp, &video_buf, &video_size) < 0 || video_buf == NULL ||
        video_size == 0) {
        unlink(video_tmp);
        unlink(audio_tmp);
        free(video_buf);
        return -1;
    }
    if (read_file_into_buffer(audio_tmp, &audio_buf, &audio_size) < 0) {
        unlink(video_tmp);
        unlink(audio_tmp);
        free(video_buf);
        free(audio_buf);
        return -1;
    }

    unlink(video_tmp);
    unlink(audio_tmp);

    if (configure_stream_source(stream, video_buf, video_size, path) < 0) {
        free(video_buf);
        free(audio_buf);
        return -1;
    }

    stream->owned_data = video_buf;
    stream->audio_data = audio_buf;
    stream->audio_size = audio_size;
    stream->has_audio_track = 0;
    if (audio_size > 0) {
        if (parse_adts_frames(stream) < 0) {
            cleanup_owned_media(stream);
            return -1;
        }
        if (stream->audio_frame_count == 0 || stream->audio_sample_rate == 0 ||
            stream->audio_samples_per_frame == 0 || stream->audio_channels == 0 ||
            stream->audio_config_hex[0] == '\0') {
            cleanup_owned_media(stream);
            return -1;
        }
        stream->has_audio_track = 1;
    }
    stream->video_fps_num = fps_num;
    stream->video_fps_den = fps_den;
    return 0;
}

void rtsp_h264_stream_source_cleanup(struct rtsp_h264_stream_source *stream) {
    cleanup_owned_media(stream);
    memset(stream, 0, sizeof(*stream));
}

size_t rtsp_h264_parse_nals_from_buffer(const uint8_t *data, size_t size,
                                        struct rtsp_h264_nal_unit *nals, size_t cap) {
    size_t count = 0;
    size_t i = 0;

    /* 扫描 Annex-B 起始码，返回去掉起始码后的 NAL 数据范围。 */
    while (i + 3 < size) {
        size_t prefix = 0;
        if (!starts_with(data, size, i, &prefix)) {
            ++i;
            continue;
        }

        size_t nal_start = i + prefix;
        size_t j = nal_start;
        while (j + 3 < size) {
            size_t next_prefix = 0;
            if (starts_with(data, size, j, &next_prefix)) {
                break;
            }
            ++j;
        }
        if (j + 3 >= size) {
            j = size;
        }
        if (count < cap) {
            nals[count].data = &data[nal_start];
            nals[count].size = j - nal_start;
            ++count;
        }
        i = j;
    }

    return count;
}
