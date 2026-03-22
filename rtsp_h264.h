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

struct rtsp_h264_stream_source {
    const uint8_t *data;
    size_t size;
    uint8_t *owned_data;
    struct rtsp_h264_frame *frames;
    size_t frame_count;
    uint8_t *audio_data;
    size_t audio_size;
    uint32_t video_fps_num;
    uint32_t video_fps_den;
    int has_audio_track;
    int synthetic_audio;
    char sprop_parameter_sets[256];
    char profile_level_id[7];
    char stream_name[256];
};

int rtsp_h264_stream_source_init_default(struct rtsp_h264_stream_source *stream);
int rtsp_h264_stream_source_load_file(struct rtsp_h264_stream_source *stream, const char *path);
int rtsp_h264_stream_source_load_mp4(struct rtsp_h264_stream_source *stream, const char *path);
int rtsp_h264_stream_source_supports_mp4(void);
void rtsp_h264_stream_source_cleanup(struct rtsp_h264_stream_source *stream);
size_t rtsp_h264_parse_nals_from_buffer(const uint8_t *data, size_t size,
                                        struct rtsp_h264_nal_unit *nals, size_t cap);

#endif
