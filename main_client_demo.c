#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rtsp_client.h"
#include "rtsp_ts_muxer.h"

static volatile sig_atomic_t g_running = 1;

struct client_demo_ctx {
    struct rtsp_ts_muxer muxer;
};

static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}

static int make_output_dir(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) == NULL) {
        return -1;
    }
    if (snprintf(out, out_size, "ts_segments_%04d%02d%02d_%02d%02d%02d", tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec) >=
        (int)out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(out, 0755) < 0) {
        return -1;
    }
    return 0;
}

static void on_frame_ex(enum rtsp_client_media_type media_type, const uint8_t *data, size_t data_size,
                        uint64_t pts90k, void *user_data) {
    struct client_demo_ctx *ctx = (struct client_demo_ctx *)user_data;
    int rc = 0;

    if (!g_running) {
        return;
    }

    if (media_type == RTSP_CLIENT_MEDIA_VIDEO) {
        rc = rtsp_ts_muxer_write_video_nal_pts(&ctx->muxer, data, data_size, pts90k);
    } else {
        rc = rtsp_ts_muxer_write_audio_block_pts(&ctx->muxer, data, data_size, pts90k);
    }
    if (rc < 0) {
        fprintf(stderr, "muxer write failed: %s\n", strerror(errno));
        g_running = 0;
    }
}

int main(int argc, char **argv) {
    struct sigaction sa;
    struct rtsp_client_config config;
    struct rtsp_ts_muxer_config muxer_config;
    struct rtsp_ts_muxer_stats stats;
    struct client_demo_ctx ctx;
    char output_dir[PATH_MAX];

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s rtsp://host:port/path [output_dir]\n", argv[0]);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (argc == 3) {
        if (snprintf(output_dir, sizeof(output_dir), "%s", argv[2]) >= (int)sizeof(output_dir)) {
            fprintf(stderr, "output dir too long\n");
            return 1;
        }
    } else if (make_output_dir(output_dir, sizeof(output_dir)) < 0) {
        fprintf(stderr, "create output dir failed: %s\n", strerror(errno));
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    memset(&muxer_config, 0, sizeof(muxer_config));
    muxer_config.output_dir = output_dir;
    muxer_config.segment_duration_ms = 2000;
    if (rtsp_ts_muxer_init(&ctx.muxer, &muxer_config) < 0) {
        fprintf(stderr, "muxer init failed: %s\n", strerror(errno));
        return 1;
    }

    printf("saving ts segments to %s\n", output_dir);
    printf("playlist path: %s/playlist.m3u8\n", output_dir);
    fflush(stdout);

    memset(&config, 0, sizeof(config));
    config.url = argv[1];
    config.on_frame_ex = on_frame_ex;
    config.user_data = &ctx;
    config.running = &g_running;

    if (rtsp_client_run(&config) < 0) {
        fprintf(stderr, "rtsp client exited with error\n");
        rtsp_ts_muxer_close(&ctx.muxer);
        return 1;
    }
    if (rtsp_ts_muxer_close(&ctx.muxer) < 0) {
        fprintf(stderr, "muxer close failed: %s\n", strerror(errno));
        return 1;
    }

    rtsp_ts_muxer_get_stats(&ctx.muxer, &stats);
    printf("saved segments=%zu video_au=%zu audio_blocks=%zu audio_frames=%zu audio_enabled=%d\n",
           stats.segment_count, stats.video_access_units, stats.audio_blocks, stats.audio_frames,
           stats.audio_enabled);
    return 0;
}
