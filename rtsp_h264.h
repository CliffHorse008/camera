#ifndef RTSP_H264_H
#define RTSP_H264_H

#include <stddef.h>
#include <stdint.h>

struct rtsp_h264_nal_unit {
    const uint8_t *data;
    size_t size;
};

struct rtsp_h264_frame {
    const uint8_t *data;
    size_t size;
};

struct rtsp_aac_frame {
    const uint8_t *data;
    size_t size;
};

struct rtsp_h264_stream_source {
    const uint8_t *data;
    size_t size;
    uint8_t *owned_data;
    struct rtsp_h264_frame *frames;
    size_t frame_count;
    uint8_t *audio_data;
    size_t audio_size;
    struct rtsp_aac_frame *audio_frames;
    size_t audio_frame_count;
    uint32_t audio_sample_rate;
    uint32_t audio_samples_per_frame;
    uint8_t audio_channels;
    uint32_t video_fps_num;
    uint32_t video_fps_den;
    int has_audio_track;
    char audio_config_hex[16];
    char sprop_parameter_sets[256];
    char profile_level_id[7];
    char stream_name[256];
};

int rtsp_h264_stream_source_load_mp4(struct rtsp_h264_stream_source *stream, const char *path);
void rtsp_h264_stream_source_cleanup(struct rtsp_h264_stream_source *stream);
size_t rtsp_h264_parse_nals_from_buffer(const uint8_t *data, size_t size,
                                        struct rtsp_h264_nal_unit *nals, size_t cap);

#endif
