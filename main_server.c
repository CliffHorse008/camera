#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rtsp_common.h"
#include "rtsp_h264.h"
#include "rtsp_server.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}

int main(int argc, char **argv) {
    struct rtsp_h264_stream_source stream;
    struct sigaction sa;
    int rc = 1;

    memset(&stream, 0, sizeof(stream));
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    srand((unsigned int)time(NULL));

    if (argc > 2) {
        fprintf(stderr, "usage: %s [file.h264]\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        if (rtsp_h264_stream_source_load_file(&stream, argv[1]) < 0) {
            fprintf(stderr, "failed to load h264 file: %s\n", argv[1]);
            goto done;
        }
    } else if (rtsp_h264_stream_source_init_default(&stream) < 0) {
        fprintf(stderr, "failed to initialize embedded h264 stream\n");
        goto done;
    }

    if (rtsp_server_run(&stream, RTSP_SERVER_PORT, &g_running) < 0) {
        perror("rtsp_server_run");
        goto done;
    }

    rc = 0;

done:
    rtsp_h264_stream_source_cleanup(&stream);
    return rc;
}
