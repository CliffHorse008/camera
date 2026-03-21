#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <signal.h>
#include <stdint.h>

#include "rtsp_h264.h"

int rtsp_server_run(const struct rtsp_h264_stream_source *stream, uint16_t port,
                    const volatile sig_atomic_t *running);

#endif
