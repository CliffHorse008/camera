#ifndef RTSP_TS_MUXER_H
#define RTSP_TS_MUXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct rtsp_ts_muxer_config {
    const char *output_dir;
    uint32_t segment_duration_ms;
};

struct rtsp_ts_muxer_stats {
    size_t video_access_units;
    size_t audio_blocks;
    size_t audio_frames;
    size_t segment_count;
    int audio_enabled;
};

struct rtsp_ts_segment_info {
    char file_name[32];
    double duration_sec;
};

struct rtsp_ts_muxer {
    char output_dir[512];
    char playlist_path[640];
    char playlist_tmp_path[640];
    uint32_t segment_duration_ms;
    FILE *segment_fp;
    char current_segment_name[32];
    int current_segment_open;
    uint64_t current_segment_start_pts;
    uint64_t latest_pts;
    uint64_t first_pts;
    int first_pts_set;
    uint8_t pat_cc;
    uint8_t pmt_cc;
    uint8_t video_cc;
    uint8_t audio_cc;
    uint32_t segment_index;
    uint32_t media_sequence;
    struct rtsp_ts_segment_info segments[1024];
    size_t segment_count;
    size_t video_access_units;
    size_t audio_blocks;
    size_t audio_frames;
    uint8_t *au_buf;
    size_t au_size;
    size_t au_cap;
    int au_has_vcl;
    int au_is_keyframe;
    uint64_t au_pts90k;
    int au_pts_valid;
    int segment_needs_codec_config;
    uint8_t sps[128];
    size_t sps_size;
    uint8_t pps[128];
    size_t pps_size;
    int audio_ready;
    uint64_t audio_next_pts;
    uint64_t audio_samples_encoded;
    uint8_t *pcm_buf;
    size_t pcm_size;
    size_t pcm_cap;
    uint8_t *pcm_native_buf;
    size_t pcm_native_cap;
    int audio_warned_ignored;
    void *aac_codec_ctx;
    void *aac_swr_ctx;
    void *aac_frame;
    void *aac_packet;
    struct timespec first_clock;
    int first_clock_set;
};

int rtsp_ts_muxer_init(struct rtsp_ts_muxer *muxer, const struct rtsp_ts_muxer_config *config);
int rtsp_ts_muxer_write_video_nal(struct rtsp_ts_muxer *muxer, const uint8_t *nal, size_t nal_size);
int rtsp_ts_muxer_write_video_nal_pts(struct rtsp_ts_muxer *muxer, const uint8_t *nal, size_t nal_size,
                                      uint64_t pts90k);
int rtsp_ts_muxer_write_audio_block(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size);
int rtsp_ts_muxer_write_audio_block_pts(struct rtsp_ts_muxer *muxer, const uint8_t *data, size_t size,
                                        uint64_t pts90k);
int rtsp_ts_muxer_close(struct rtsp_ts_muxer *muxer);
void rtsp_ts_muxer_get_stats(const struct rtsp_ts_muxer *muxer, struct rtsp_ts_muxer_stats *stats);

#endif
