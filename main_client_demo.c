#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtsp_client.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signo) {
    (void)signo;
    g_running = 0;
}

static void on_frame(enum rtsp_client_media_type media_type, const uint8_t *data, size_t data_size,
                     void *user_data) {
    size_t *count = (size_t *)user_data;
    (*count)++;
    if (media_type == RTSP_CLIENT_MEDIA_VIDEO) {
        uint8_t nal_type = data_size > 0 ? (uint8_t)(data[0] & 0x1f) : 0;
        printf("VIDEO #%zu nal_type=%u size=%zu\n", *count, nal_type, data_size);
    } else {
        printf("AUDIO #%zu pcm_bytes=%zu\n", *count, data_size);
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    size_t count = 0;
    struct sigaction sa;
    struct rtsp_client_config config;

    if (argc != 2) {
        fprintf(stderr, "usage: %s rtsp://host:port/path\n", argv[0]);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&config, 0, sizeof(config));
    config.url = argv[1];
    config.on_frame = on_frame;
    config.user_data = &count;
    config.running = &g_running;

    return rtsp_client_run(&config) == 0 ? 0 : 1;
}
