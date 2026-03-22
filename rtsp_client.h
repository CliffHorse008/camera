#ifndef RTSP_CLIENT_H
#define RTSP_CLIENT_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

enum rtsp_client_media_type {
    RTSP_CLIENT_MEDIA_VIDEO = 0,
    RTSP_CLIENT_MEDIA_AUDIO = 1,
};

typedef void (*rtsp_client_frame_callback)(enum rtsp_client_media_type media_type, const uint8_t *data,
                                           size_t data_size, void *user_data);
typedef void (*rtsp_client_frame_callback_ex)(enum rtsp_client_media_type media_type, const uint8_t *data,
                                              size_t data_size, uint64_t pts90k, void *user_data);

struct rtsp_client_config {
    const char *url;
    rtsp_client_frame_callback on_frame;
    rtsp_client_frame_callback_ex on_frame_ex;
    void *user_data;
    const volatile sig_atomic_t *running;
};

int rtsp_client_run(const struct rtsp_client_config *config);

#endif
