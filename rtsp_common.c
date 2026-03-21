#include "rtsp_common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static const char *find_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p != '\0'; ++p) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}

int rtsp_parse_url(const char *url, struct rtsp_url *out) {
    const char *prefix = "rtsp://";
    const char *host_begin = NULL;
    const char *path_begin = NULL;
    const char *colon = NULL;
    size_t host_len = 0;
    size_t path_len = 0;

    if (url == NULL || out == NULL) {
        return -1;
    }
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->url, sizeof(out->url), "%s", url);

    /* 将 rtsp://host[:port]/path 拆分到固定字段中，供后续流程使用。 */
    host_begin = url + strlen(prefix);
    path_begin = strchr(host_begin, '/');
    if (path_begin == NULL) {
        path_begin = host_begin + strlen(host_begin);
    }

    colon = memchr(host_begin, ':', (size_t)(path_begin - host_begin));
    if (colon != NULL) {
        host_len = (size_t)(colon - host_begin);
        out->port = (uint16_t)atoi(colon + 1);
    } else {
        host_len = (size_t)(path_begin - host_begin);
        out->port = RTSP_DEFAULT_PORT;
    }

    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, host_begin, host_len);
    out->host[host_len] = '\0';

    if (*path_begin == '\0') {
        snprintf(out->path, sizeof(out->path), "/");
    } else {
        path_len = strlen(path_begin);
        if (path_len >= sizeof(out->path)) {
            return -1;
        }
        memcpy(out->path, path_begin, path_len);
        out->path[path_len] = '\0';
    }

    if (out->port == 0) {
        out->port = RTSP_DEFAULT_PORT;
    }
    return 0;
}

int rtsp_tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    char port_str[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return -1;
    }

    /* 依次尝试解析出的地址，直到有一个连接成功。 */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

int rtsp_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        /* RTSP 响应和 TCP 复用的 RTP 帧都必须完整写出。 */
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

int rtsp_recv_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        /* 用于读取固定长度数据，比如 interleaved RTP 帧头。 */
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

int rtsp_find_content_length(const char *message) {
    char value[32];
    if (rtsp_find_header(message, "Content-Length", value, sizeof(value)) < 0) {
        return 0;
    }
    return atoi(value);
}

int rtsp_read_message(int fd, char *buf, size_t cap, size_t *out_len) {
    size_t used = 0;
    int content_length = 0;
    char *headers_end = NULL;

    if (cap == 0) {
        return -1;
    }

    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        used += (size_t)n;
        buf[used] = '\0';

        headers_end = strstr(buf, "\r\n\r\n");
        if (headers_end == NULL) {
            continue;
        }

        /* RTSP 报文结束位置等于头结束位置加上可选消息体长度。 */
        content_length = rtsp_find_content_length(buf);
        if ((size_t)(headers_end + 4 - buf) + (size_t)content_length <= used) {
            if (out_len != NULL) {
                *out_len = used;
            }
            return 0;
        }
    }

    return -1;
}

int rtsp_find_header(const char *message, const char *name, char *out, size_t out_size) {
    char needle[128];
    const char *line = NULL;
    const char *colon = NULL;
    const char *line_end = NULL;
    size_t len = 0;

    /* 兼容匹配首个头字段和后续头字段。 */
    snprintf(needle, sizeof(needle), "\n%s:", name);
    line = find_case_insensitive(message, needle);
    if (line != NULL) {
        line += 1;
    } else {
        snprintf(needle, sizeof(needle), "%s:", name);
        line = find_case_insensitive(message, needle);
    }
    if (line == NULL) {
        return -1;
    }

    colon = strchr(line, ':');
    if (colon == NULL) {
        return -1;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t') {
        colon++;
    }

    line_end = strstr(colon, "\r\n");
    if (line_end == NULL) {
        line_end = strchr(colon, '\n');
    }
    if (line_end == NULL) {
        line_end = colon + strlen(colon);
    }

    len = (size_t)(line_end - colon);
    while (len > 0 && isspace((unsigned char)colon[len - 1])) {
        len--;
    }
    if (len + 1 > out_size) {
        return -1;
    }
    memcpy(out, colon, len);
    out[len] = '\0';
    return 0;
}

void rtsp_extract_request_url(const char *request, char *url, size_t url_size) {
    const char *line_end = strstr(request, "\r\n");
    const char *sp1 = NULL;
    const char *sp2 = NULL;
    size_t len = 0;

    if (line_end == NULL) {
        line_end = strchr(request, '\n');
    }
    if (line_end == NULL) {
        snprintf(url, url_size, "rtsp://0.0.0.0:%d/live", RTSP_SERVER_PORT);
        return;
    }

    sp1 = strchr(request, ' ');
    if (sp1 == NULL || sp1 >= line_end) {
        snprintf(url, url_size, "rtsp://0.0.0.0:%d/live", RTSP_SERVER_PORT);
        return;
    }
    sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL || sp2 > line_end) {
        snprintf(url, url_size, "rtsp://0.0.0.0:%d/live", RTSP_SERVER_PORT);
        return;
    }

    len = (size_t)(sp2 - (sp1 + 1));
    if (len >= url_size) {
        len = url_size - 1;
    }
    memcpy(url, sp1 + 1, len);
    url[len] = '\0';
}

int rtsp_build_control_url(const char *base_url, const char *control, char *out, size_t out_size) {
    const char *last_slash = NULL;

    if (strncmp(control, "rtsp://", 7) == 0) {
        snprintf(out, out_size, "%s", control);
        return 0;
    }

    if (strcmp(control, "*") == 0) {
        snprintf(out, out_size, "%s", base_url);
        return 0;
    }

    if (control[0] == '/') {
        struct rtsp_url parsed;
        if (rtsp_parse_url(base_url, &parsed) < 0) {
            return -1;
        }
        snprintf(out, out_size, "rtsp://%s:%u%s", parsed.host, parsed.port, control);
        return 0;
    }

    last_slash = strrchr(base_url, '/');
    if (last_slash == NULL) {
        return -1;
    }
    snprintf(out, out_size, "%.*s/%s", (int)(last_slash - base_url), base_url, control);
    return 0;
}
