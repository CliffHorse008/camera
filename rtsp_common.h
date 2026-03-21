#ifndef RTSP_COMMON_H
#define RTSP_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define RTSP_DEFAULT_PORT 554
#define RTSP_SERVER_PORT 5554
#define RTSP_MAX_MESSAGE 4096
#define RTP_CLOCK_H264 90000
#define RTP_PAYLOAD_TYPE_H264 96
#define RTP_MTU_PAYLOAD 1400

struct rtsp_url {
    char host[256];
    uint16_t port;
    char path[256];
    char url[512];
};

/* 解析完整 RTSP URL，拆出 host/port/path。 */
int rtsp_parse_url(const char *url, struct rtsp_url *out);
/* 建立 RTSP 控制连接使用的 TCP socket。 */
int rtsp_tcp_connect(const char *host, uint16_t port);
/* 即使 send() 发生短写，也要把整个缓冲区发完。 */
int rtsp_send_all(int fd, const void *buf, size_t len);
/* 精确读取 len 字节，否则返回失败。 */
int rtsp_recv_exact(int fd, void *buf, size_t len);
/* 读取一条完整 RTSP 报文，包含可选消息体。 */
int rtsp_read_message(int fd, char *buf, size_t cap, size_t *out_len);
/* 不区分大小写查找 RTSP 头字段。 */
int rtsp_find_header(const char *message, const char *name, char *out, size_t out_size);
int rtsp_find_content_length(const char *message);
/* 从 RTSP 请求首行里提取请求 URL。 */
void rtsp_extract_request_url(const char *request, char *url, size_t url_size);
/* 根据 SDP 的 Content-Base 解析 track control URL。 */
int rtsp_build_control_url(const char *base_url, const char *control, char *out, size_t out_size);

#endif
