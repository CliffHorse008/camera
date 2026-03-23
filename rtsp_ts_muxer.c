#include "rtsp_ts_muxer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TS_PACKET_SIZE 188
#define TS_PAT_PID 0x0000
#define TS_PMT_PID 0x1000
#define TS_VIDEO_PID 0x0100
#define TS_PTS_HZ 90000ULL
#define TS_STREAM_ID_VIDEO 0xE0
#define TS_STREAM_TYPE_H264 0x1B
#define TS_PROGRAM_NUMBER 1
#define TS_MAX_SEGMENTS 1024
#define TS_AUDIO_PID 0x0101
#define TS_STREAM_ID_AUDIO 0xC0
#define TS_STREAM_TYPE_AAC 0x0F

static uint32_t crc32_mpeg(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffU;

    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000U) {
                crc = (crc << 1) ^ 0x04c11db7U;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint64_t now_pts90k(struct rtsp_ts_muxer *muxer) {
    struct timespec now;
    uint64_t delta_ns = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return muxer->latest_pts;
    }
    if (!muxer->first_clock_set) {
        muxer->first_clock = now;
        muxer->first_clock_set = 1;
        return 0;
    }

    delta_ns = (uint64_t)(now.tv_sec - muxer->first_clock.tv_sec) * 1000000000ULL;
    delta_ns += (uint64_t)(now.tv_nsec - muxer->first_clock.tv_nsec);
    return (delta_ns * TS_PTS_HZ) / 1000000000ULL;
}

static int ensure_au_capacity(struct rtsp_ts_muxer *muxer, size_t needed) {
    uint8_t *new_buf = NULL;
    size_t new_cap = muxer->au_cap == 0 ? 4096 : muxer->au_cap;

    while (new_cap < needed) {
        new_cap *= 2;
    }
    if (new_cap == muxer->au_cap) {
        return 0;
    }
    new_buf = (uint8_t *)realloc(muxer->au_buf, new_cap);
    if (new_buf == NULL) {
        return -1;
    }
    muxer->au_buf = new_buf;
    muxer->au_cap = new_cap;
    return 0;
}

static int au_append_nal(struct rtsp_ts_muxer *muxer, const uint8_t *nal, size_t nal_size) {
    static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

    if (ensure_au_capacity(muxer, muxer->au_size + sizeof(start_code) + nal_size) < 0) {
        return -1;
    }
    memcpy(muxer->au_buf + muxer->au_size, start_code, sizeof(start_code));
    muxer->au_size += sizeof(start_code);
    memcpy(muxer->au_buf + muxer->au_size, nal, nal_size);
    muxer->au_size += nal_size;
    return 0;
}

static void cache_codec_nal(struct rtsp_ts_muxer *muxer, uint8_t nal_type, const uint8_t *nal, size_t nal_size) {
    if (nal_type == 7 && nal_size <= sizeof(muxer->sps)) {
        memcpy(muxer->sps, nal, nal_size);
        muxer->sps_size = nal_size;
    } else if (nal_type == 8 && nal_size <= sizeof(muxer->pps)) {
        memcpy(muxer->pps, nal, nal_size);
        muxer->pps_size = nal_size;
    }
}

static void write_pts_field(uint8_t *dst, uint8_t marker_bits, uint64_t pts) {
    dst[0] = (uint8_t)((marker_bits << 4) | (((pts >> 30) & 0x07) << 1) | 1);
    dst[1] = (uint8_t)(pts >> 22);
    dst[2] = (uint8_t)((((pts >> 15) & 0x7f) << 1) | 1);
    dst[3] = (uint8_t)(pts >> 7);
    dst[4] = (uint8_t)(((pts & 0x7f) << 1) | 1);
}

static void write_pcr_field(uint8_t *dst, uint64_t pcr_base) {
    uint64_t base = pcr_base & 0x1ffffffffULL;

    dst[0] = (uint8_t)(base >> 25);
    dst[1] = (uint8_t)(base >> 17);
    dst[2] = (uint8_t)(base >> 9);
    dst[3] = (uint8_t)(base >> 1);
    dst[4] = (uint8_t)((base & 0x01) << 7);
    dst[5] = 0x00;
}

static int write_ts_packet(FILE *fp, uint16_t pid, int payload_unit_start, uint8_t continuity_counter,
                           int with_pcr, uint64_t pcr_base, const uint8_t *payload, size_t payload_size,
                           size_t *consumed_out) {
    uint8_t packet[TS_PACKET_SIZE];
    size_t payload_offset = 4;
    size_t payload_cap = TS_PACKET_SIZE - 4;
    size_t to_copy = 0;
    int adaptation_control = 1;

    memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47;
    packet[1] = (uint8_t)(((payload_unit_start ? 0x40 : 0x00) | ((pid >> 8) & 0x1f)));
    packet[2] = (uint8_t)(pid & 0xff);

    if (with_pcr) {
        size_t adaptation_field_length = 7;
        size_t total_adaptation = 1 + adaptation_field_length;

        adaptation_control = 3;
        if (payload_size < payload_cap - total_adaptation) {
            adaptation_field_length += payload_cap - total_adaptation - payload_size;
            total_adaptation = 1 + adaptation_field_length;
        }
        packet[4] = (uint8_t)adaptation_field_length;
        packet[5] = with_pcr ? 0x10 : 0x00;
        write_pcr_field(packet + 6, pcr_base);
        payload_offset += total_adaptation;
        payload_cap -= total_adaptation;
    } else if (payload_size < payload_cap) {
        size_t adaptation_field_length = payload_cap - payload_size - 1;

        adaptation_control = 3;
        packet[4] = (uint8_t)adaptation_field_length;
        if (adaptation_field_length > 0) {
            packet[5] = 0x00;
        }
        payload_offset += 1 + adaptation_field_length;
        payload_cap = TS_PACKET_SIZE - payload_offset;
    }

    packet[3] = (uint8_t)((adaptation_control << 4) | (continuity_counter & 0x0f));
    to_copy = payload_size < payload_cap ? payload_size : payload_cap;
    memcpy(packet + payload_offset, payload, to_copy);

    if (fwrite(packet, 1, sizeof(packet), fp) != sizeof(packet)) {
        return -1;
    }
    if (consumed_out != NULL) {
        *consumed_out = to_copy;
    }
    return 0;
}

static int write_pat(struct rtsp_ts_muxer *muxer) {
    uint8_t section[32];
    uint32_t crc = 0;
    size_t used = 0;

    memset(section, 0, sizeof(section));
    section[used++] = 0x00;
    section[used++] = 0xB0;
    section[used++] = 0x0D;
    section[used++] = 0x00;
    section[used++] = TS_PROGRAM_NUMBER;
    section[used++] = 0xC1;
    section[used++] = 0x00;
    section[used++] = 0x00;
    section[used++] = 0x00;
    section[used++] = TS_PROGRAM_NUMBER;
    section[used++] = 0xE0 | ((TS_PMT_PID >> 8) & 0x1f);
    section[used++] = (uint8_t)(TS_PMT_PID & 0xff);
    crc = crc32_mpeg(section, used);
    section[used++] = (uint8_t)(crc >> 24);
    section[used++] = (uint8_t)(crc >> 16);
    section[used++] = (uint8_t)(crc >> 8);
    section[used++] = (uint8_t)crc;

    {
        uint8_t payload[TS_PACKET_SIZE - 4];
        size_t consumed = 0;

        memset(payload, 0xff, sizeof(payload));
        payload[0] = 0x00;
        memcpy(payload + 1, section, used);
        if (write_ts_packet(muxer->segment_fp, TS_PAT_PID, 1, muxer->pat_cc++, 0, 0, payload, used + 1,
                            &consumed) < 0) {
            return -1;
        }
        (void)consumed;
    }

    return 0;
}

static int write_pmt(struct rtsp_ts_muxer *muxer) {
    uint8_t section[64];
    uint32_t crc = 0;
    size_t used = 0;

    memset(section, 0, sizeof(section));
    section[used++] = 0x02;
    section[used++] = 0xB0;
    section[used++] = 0x12;
    section[used++] = 0x00;
    section[used++] = TS_PROGRAM_NUMBER;
    section[used++] = 0xC1;
    section[used++] = 0x00;
    section[used++] = 0x00;
    section[used++] = 0xE0 | ((TS_VIDEO_PID >> 8) & 0x1f);
    section[used++] = (uint8_t)(TS_VIDEO_PID & 0xff);
    section[used++] = 0xF0;
    section[used++] = 0x00;
    section[used++] = TS_STREAM_TYPE_H264;
    section[used++] = 0xE0 | ((TS_VIDEO_PID >> 8) & 0x1f);
    section[used++] = (uint8_t)(TS_VIDEO_PID & 0xff);
    section[used++] = 0xF0;
    section[used++] = 0x00;
    section[used++] = TS_STREAM_TYPE_AAC;
    section[used++] = 0xE0 | ((TS_AUDIO_PID >> 8) & 0x1f);
    section[used++] = (uint8_t)(TS_AUDIO_PID & 0xff);
    section[used++] = 0xF0;
    section[used++] = 0x00;
    section[2] = 0x17;
    crc = crc32_mpeg(section, used);
    section[used++] = (uint8_t)(crc >> 24);
    section[used++] = (uint8_t)(crc >> 16);
    section[used++] = (uint8_t)(crc >> 8);
    section[used++] = (uint8_t)crc;

    {
        uint8_t payload[TS_PACKET_SIZE - 4];
        size_t consumed = 0;

        memset(payload, 0xff, sizeof(payload));
        payload[0] = 0x00;
        memcpy(payload + 1, section, used);
        if (write_ts_packet(muxer->segment_fp, TS_PMT_PID, 1, muxer->pmt_cc++, 0, 0, payload, used + 1,
                            &consumed) < 0) {
            return -1;
        }
        (void)consumed;
    }

    return 0;
}

static int playlist_write(struct rtsp_ts_muxer *muxer, int endlist) {
    FILE *fp = fopen(muxer->playlist_tmp_path, "wb");
    double max_duration = 0.0;
    unsigned target_duration = 2;

    if (fp == NULL) {
        return -1;
    }

    for (size_t i = 0; i < muxer->segment_count; ++i) {
        if (muxer->segments[i].duration_sec > max_duration) {
            max_duration = muxer->segments[i].duration_sec;
        }
    }
    if (max_duration > 0.0) {
        target_duration = (unsigned)(max_duration + 0.999);
    }

    fprintf(fp, "#EXTM3U\n");
    fprintf(fp, "#EXT-X-VERSION:3\n");
    fprintf(fp, "#EXT-X-TARGETDURATION:%u\n", target_duration);
    fprintf(fp, "#EXT-X-MEDIA-SEQUENCE:%u\n", muxer->media_sequence);
    for (size_t i = 0; i < muxer->segment_count; ++i) {
        fprintf(fp, "#EXTINF:%.3f,\n", muxer->segments[i].duration_sec);
        fprintf(fp, "%s\n", muxer->segments[i].file_name);
    }
    if (endlist) {
        fprintf(fp, "#EXT-X-ENDLIST\n");
    }

    if (fclose(fp) != 0) {
        unlink(muxer->playlist_tmp_path);
        return -1;
    }
    if (rename(muxer->playlist_tmp_path, muxer->playlist_path) < 0) {
        unlink(muxer->playlist_tmp_path);
        return -1;
    }
    return 0;
}

static int close_segment(struct rtsp_ts_muxer *muxer, uint64_t end_pts) {
    double duration_sec = 0.0;

    if (!muxer->current_segment_open) {
        return 0;
    }
    if (end_pts > muxer->current_segment_start_pts) {
        duration_sec = (double)(end_pts - muxer->current_segment_start_pts) / (double)TS_PTS_HZ;
    }
    if (duration_sec <= 0.0) {
        duration_sec = (double)muxer->segment_duration_ms / 1000.0;
    }

    if (fclose(muxer->segment_fp) != 0) {
        muxer->segment_fp = NULL;
        muxer->current_segment_open = 0;
        return -1;
    }
    muxer->segment_fp = NULL;
    muxer->current_segment_open = 0;

    if (muxer->segment_count < TS_MAX_SEGMENTS) {
        struct rtsp_ts_segment_info *info = &muxer->segments[muxer->segment_count++];
        snprintf(info->file_name, sizeof(info->file_name), "%s", muxer->current_segment_name);
        info->duration_sec = duration_sec;
    }

    if (playlist_write(muxer, 0) < 0) {
        return -1;
    }
    return 0;
}

static int open_segment(struct rtsp_ts_muxer *muxer, uint64_t start_pts) {
    char path[640];

    if (snprintf(muxer->current_segment_name, sizeof(muxer->current_segment_name), "segment_%05u.ts",
                 muxer->segment_index++) >= (int)sizeof(muxer->current_segment_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%s", muxer->output_dir, muxer->current_segment_name) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    muxer->segment_fp = fopen(path, "wb");
    if (muxer->segment_fp == NULL) {
        return -1;
    }
    muxer->current_segment_open = 1;
    muxer->current_segment_start_pts = start_pts;
    muxer->segment_needs_codec_config = 1;
    muxer->pat_cc = 0;
    muxer->pmt_cc = 0;
    muxer->video_cc = 0;
    muxer->audio_cc = 0;

    if (write_pat(muxer) < 0 || write_pmt(muxer) < 0) {
        fclose(muxer->segment_fp);
        muxer->segment_fp = NULL;
        muxer->current_segment_open = 0;
        return -1;
    }
    return 0;
}

static int write_video_pes(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size, uint64_t pts) {
    uint8_t header[32];
    size_t header_size = 14;
    size_t payload_consumed = 0;
    size_t first_payload = 0;
    uint16_t pes_packet_length = 0;

    if (size + 8 < 0xffff) {
        pes_packet_length = (uint16_t)(size + 8);
    }

    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = TS_STREAM_ID_VIDEO;
    header[4] = (uint8_t)(pes_packet_length >> 8);
    header[5] = (uint8_t)pes_packet_length;
    header[6] = 0x80;
    header[7] = 0x80;
    header[8] = 0x05;
    write_pts_field(header + 9, 0x02, pts);

    {
        uint8_t *pes = (uint8_t *)malloc(header_size + size);
        size_t pes_size = header_size + size;

        if (pes == NULL) {
            return -1;
        }
        memcpy(pes, header, header_size);
        memcpy(pes + header_size, data, size);

        if (write_ts_packet(muxer->segment_fp, TS_VIDEO_PID, 1, muxer->video_cc++, 1, pts * 300ULL, pes,
                            pes_size, &first_payload) < 0) {
            free(pes);
            return -1;
        }
        payload_consumed = first_payload;
        while (payload_consumed < pes_size) {
            size_t consumed = 0;
            if (write_ts_packet(muxer->segment_fp, TS_VIDEO_PID, 0, muxer->video_cc++, 0, 0,
                                pes + payload_consumed, pes_size - payload_consumed, &consumed) < 0) {
                free(pes);
                return -1;
            }
            payload_consumed += consumed;
        }
        free(pes);
    }

    return 0;
}

static int write_pending_codec_config(struct rtsp_ts_muxer *muxer, uint64_t pts) {
    uint8_t config[512];
    size_t used = 0;
    static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

    if (!muxer->segment_needs_codec_config) {
        return 0;
    }
    if (muxer->sps_size == 0 || muxer->pps_size == 0) {
        muxer->segment_needs_codec_config = 0;
        return 0;
    }
    if (sizeof(start_code) + muxer->sps_size + sizeof(start_code) + muxer->pps_size > sizeof(config)) {
        errno = EOVERFLOW;
        return -1;
    }

    memcpy(config + used, start_code, sizeof(start_code));
    used += sizeof(start_code);
    memcpy(config + used, muxer->sps, muxer->sps_size);
    used += muxer->sps_size;
    memcpy(config + used, start_code, sizeof(start_code));
    used += sizeof(start_code);
    memcpy(config + used, muxer->pps, muxer->pps_size);
    used += muxer->pps_size;

    if (write_video_pes(muxer, config, used, pts) < 0) {
        return -1;
    }
    muxer->segment_needs_codec_config = 0;
    return 0;
}

static int write_audio_pes(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size, uint64_t pts) {
    uint8_t header[32];
    size_t header_size = 14;
    size_t payload_consumed = 0;
    size_t first_payload = 0;
    uint16_t pes_packet_length = 0;

    if (size + 8 < 0xffff) {
        pes_packet_length = (uint16_t)(size + 8);
    }

    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = TS_STREAM_ID_AUDIO;
    header[4] = (uint8_t)(pes_packet_length >> 8);
    header[5] = (uint8_t)pes_packet_length;
    header[6] = 0x80;
    header[7] = 0x80;
    header[8] = 0x05;
    write_pts_field(header + 9, 0x02, pts);

    {
        uint8_t *pes = (uint8_t *)malloc(header_size + size);
        size_t pes_size = header_size + size;

        if (pes == NULL) {
            return -1;
        }
        memcpy(pes, header, header_size);
        memcpy(pes + header_size, data, size);

        if (write_ts_packet(muxer->segment_fp, TS_AUDIO_PID, 1, muxer->audio_cc++, 0, 0, pes, pes_size,
                            &first_payload) < 0) {
            free(pes);
            return -1;
        }
        payload_consumed = first_payload;
        while (payload_consumed < pes_size) {
            size_t consumed = 0;
            if (write_ts_packet(muxer->segment_fp, TS_AUDIO_PID, 0, muxer->audio_cc++, 0, 0,
                                pes + payload_consumed, pes_size - payload_consumed, &consumed) < 0) {
                free(pes);
                return -1;
            }
            payload_consumed += consumed;
        }
        free(pes);
    }

    return 0;
}

static int read_ue(const uint8_t *data, size_t size, size_t *bit_offset, unsigned *value) {
    unsigned zeros = 0;
    unsigned code_num = 0;

    while (*bit_offset < size * 8) {
        uint8_t bit = (uint8_t)((data[*bit_offset / 8] >> (7 - (*bit_offset % 8))) & 0x01);
        (*bit_offset)++;
        if (bit != 0) {
            break;
        }
        zeros++;
    }

    code_num = 1;
    while (zeros > 0 && *bit_offset < size * 8) {
        code_num = (code_num << 1) | ((data[*bit_offset / 8] >> (7 - (*bit_offset % 8))) & 0x01);
        (*bit_offset)++;
        zeros--;
    }
    *value = code_num - 1;
    return 0;
}

static int extract_rbsp(const uint8_t *nal, size_t nal_size, uint8_t *rbsp, size_t rbsp_cap, size_t *rbsp_size) {
    size_t out = 0;
    int zero_count = 0;

    for (size_t i = 1; i < nal_size; ++i) {
        uint8_t byte = nal[i];
        if (zero_count == 2 && byte == 0x03) {
            zero_count = 0;
            continue;
        }
        if (out >= rbsp_cap) {
            return -1;
        }
        rbsp[out++] = byte;
        if (byte == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }
    *rbsp_size = out;
    return 0;
}

static int is_new_access_unit(const uint8_t *nal, size_t nal_size) {
    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    size_t bit_offset = 0;
    unsigned first_mb_in_slice = 0;

    if (nal_size < 2) {
        return 0;
    }
    if (extract_rbsp(nal, nal_size, rbsp, sizeof(rbsp), &rbsp_size) < 0) {
        return 0;
    }
    if (read_ue(rbsp, rbsp_size, &bit_offset, &first_mb_in_slice) < 0) {
        return 0;
    }
    return first_mb_in_slice == 0;
}

static int flush_access_unit(struct rtsp_ts_muxer *muxer) {
    uint64_t pts = 0;
    uint64_t elapsed_pts = 0;
    uint64_t cut_pts = ((uint64_t)muxer->segment_duration_ms * TS_PTS_HZ) / 1000ULL;

    if (muxer->au_size == 0 || !muxer->au_has_vcl) {
        return 0;
    }

    pts = muxer->au_pts_valid ? muxer->au_pts90k : now_pts90k(muxer);
    if (!muxer->first_pts_set) {
        muxer->first_pts = pts;
        muxer->first_pts_set = 1;
    }
    muxer->latest_pts = pts;

    if (!muxer->current_segment_open) {
        if (open_segment(muxer, pts) < 0) {
            return -1;
        }
    } else {
        elapsed_pts = pts - muxer->current_segment_start_pts;
        if (elapsed_pts >= cut_pts && muxer->au_is_keyframe) {
            if (close_segment(muxer, pts) < 0 || open_segment(muxer, pts) < 0) {
                return -1;
            }
        }
    }
    if (write_pending_codec_config(muxer, pts) < 0) {
        return -1;
    }
    if (write_video_pes(muxer, muxer->au_buf, muxer->au_size, pts) < 0) {
        return -1;
    }

    muxer->video_access_units++;
    muxer->au_size = 0;
    muxer->au_has_vcl = 0;
    muxer->au_is_keyframe = 0;
    muxer->au_pts_valid = 0;
    return 0;
}

int rtsp_ts_muxer_init(struct rtsp_ts_muxer *muxer, const struct rtsp_ts_muxer_config *config) {
    if (muxer == NULL || config == NULL || config->output_dir == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(muxer, 0, sizeof(*muxer));
    muxer->segment_duration_ms = config->segment_duration_ms == 0 ? 2000 : config->segment_duration_ms;
    if (snprintf(muxer->output_dir, sizeof(muxer->output_dir), "%s", config->output_dir) >=
        (int)sizeof(muxer->output_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(muxer->output_dir, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    if (snprintf(muxer->playlist_path, sizeof(muxer->playlist_path), "%s/playlist.m3u8",
                 muxer->output_dir) >= (int)sizeof(muxer->playlist_path) ||
        snprintf(muxer->playlist_tmp_path, sizeof(muxer->playlist_tmp_path), "%s/playlist.tmp",
                 muxer->output_dir) >= (int)sizeof(muxer->playlist_tmp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (playlist_write(muxer, 0) < 0) {
        return -1;
    }
    return 0;
}

int rtsp_ts_muxer_write_video_nal_pts(struct rtsp_ts_muxer *muxer, const uint8_t *nal, size_t nal_size,
                                      uint64_t pts90k) {
    uint8_t nal_type = 0;

    if (muxer == NULL || nal == NULL || nal_size == 0) {
        errno = EINVAL;
        return -1;
    }

    nal_type = (uint8_t)(nal[0] & 0x1f);
    if (nal_type == 7 || nal_type == 8) {
        cache_codec_nal(muxer, nal_type, nal, nal_size);
    }
    if (nal_type == 9 && muxer->au_has_vcl) {
        if (flush_access_unit(muxer) < 0) {
            return -1;
        }
    }
    if ((nal_type == 1 || nal_type == 5) && muxer->au_has_vcl && is_new_access_unit(nal, nal_size)) {
        if (flush_access_unit(muxer) < 0) {
            return -1;
        }
    }
    if (au_append_nal(muxer, nal, nal_size) < 0) {
        return -1;
    }
    if (!muxer->au_pts_valid) {
        muxer->au_pts90k = pts90k;
        muxer->au_pts_valid = 1;
    }
    if (nal_type == 1 || nal_type == 5) {
        muxer->au_has_vcl = 1;
        if (nal_type == 5) {
            muxer->au_is_keyframe = 1;
        }
    }
    return 0;
}

int rtsp_ts_muxer_write_video_nal(struct rtsp_ts_muxer *muxer, const uint8_t *nal, size_t nal_size) {
    return rtsp_ts_muxer_write_video_nal_pts(muxer, nal, nal_size, 0);
}

int rtsp_ts_muxer_write_audio_block_pts(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size,
                                        uint64_t pts90k) {
    uint64_t current_pts = 0;

    if (muxer == NULL || data == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    muxer->audio_blocks++;
    if (!muxer->first_pts_set) {
        muxer->audio_next_pts = pts90k != 0 ? pts90k : now_pts90k(muxer);
        muxer->first_pts = muxer->audio_next_pts;
        muxer->first_pts_set = 1;
        muxer->latest_pts = muxer->audio_next_pts;
    }
    current_pts = pts90k != 0 ? pts90k : muxer->audio_next_pts;
    muxer->audio_ready = 1;
    if (!muxer->current_segment_open) {
        muxer->audio_next_pts = current_pts;
        return 0;
    }
    if (write_audio_pes(muxer, data, size, current_pts) < 0) {
        return -1;
    }
    muxer->audio_frames++;
    muxer->latest_pts = current_pts > muxer->latest_pts ? current_pts : muxer->latest_pts;
    muxer->audio_next_pts = current_pts;
    return 0;
}

int rtsp_ts_muxer_write_audio_block(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size) {
    return rtsp_ts_muxer_write_audio_block_pts(muxer, data, size, 0);
}

int rtsp_ts_muxer_close(struct rtsp_ts_muxer *muxer) {
    if (muxer == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (flush_access_unit(muxer) < 0) {
        return -1;
    }
    if (close_segment(muxer, muxer->latest_pts) < 0) {
        return -1;
    }
    if (playlist_write(muxer, 1) < 0) {
        return -1;
    }
    free(muxer->au_buf);
    muxer->au_buf = NULL;
    muxer->au_cap = 0;
    muxer->au_size = 0;
    return 0;
}

void rtsp_ts_muxer_get_stats(const struct rtsp_ts_muxer *muxer, struct rtsp_ts_muxer_stats *stats) {
    if (muxer == NULL || stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->video_access_units = muxer->video_access_units;
    stats->audio_blocks = muxer->audio_blocks;
    stats->audio_frames = muxer->audio_frames;
    stats->segment_count = muxer->segment_count;
    stats->audio_enabled = muxer->audio_ready;
}
