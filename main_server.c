#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>

#include "rtsp_common.h"
#include "rtsp_h264.h"
#include "rtsp_server.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}

static void print_usage(FILE *fp, const char *prog) {
    fprintf(fp,
            "Usage: %s --input PATH.mp4\n"
            "       %s PATH.mp4\n"
            "\n"
            "Options:\n"
            "  -i, --input PATH   Input MP4 media file\n"
            "  -h, --help         Show this help message\n",
            prog, prog);
}

int main(int argc, char **argv) {
    struct rtsp_h264_stream_source stream;
    struct sigaction sa;
    const char *input_path = NULL;
    int rc = 1;
    int opt = 0;
    static const struct option long_options[] = {
        {"input", required_argument, NULL, 'i'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    memset(&stream, 0, sizeof(stream));
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    srand((unsigned int)time(NULL));

    while ((opt = getopt_long(argc, argv, "i:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                input_path = optarg;
                break;
            case 'h':
                print_usage(stdout, argv[0]);
                return 0;
            default:
                print_usage(stderr, argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        if (input_path != NULL || optind + 1 != argc) {
            print_usage(stderr, argv[0]);
            return 1;
        }
        input_path = argv[optind];
    }

    if (input_path == NULL) {
        fprintf(stderr, "missing input mp4 file; pass --input PATH.mp4\n");
        goto done;
    }

    const char *ext = strrchr(input_path, '.');
    if (ext == NULL || strcasecmp(ext, ".mp4") != 0) {
        fprintf(stderr, "unsupported input type: %s (only .mp4 is allowed)\n", input_path);
        goto done;
    }

    if (rtsp_h264_stream_source_load_mp4(&stream, input_path) < 0) {
        fprintf(stderr, "failed to load mp4 file: %s\n", input_path);
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
