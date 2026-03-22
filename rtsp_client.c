#include "rtsp_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "rtsp_common.h"

struct client_track {
    enum rtsp_client_media_type media_type;
    char control_url[512];
    uint8_t payload_type;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    uint32_t clock_rate;
    uint32_t first_rtp_timestamp;
    int first_rtp_timestamp_set;
    uint8_t sps[128];
    size_t sps_size;
    uint8_t pps[128];
    size_t pps_size;
    int codec_config_sent;
    uint8_t *fu_buf;
    size_t fu_size;
    size_t fu_cap;
    int configured;
};

struct rtsp_client_ctx {
    int fd;
    int cseq;
    char session_id[128];
    struct rtsp_url parsed_url;
    uint8_t rx_buf[RTSP_MAX_MESSAGE * 4 + 1];
    size_t rx_len;
    struct client_track tracks[2];
    rtsp_client_frame_callback on_frame;
    rtsp_client_frame_callback_ex on_frame_ex;
    void *user_data;
};

static void client_emit_frame(struct rtsp_client_ctx *ctx, enum rtsp_client_media_type media_type,
                              const uint8_t *data, size_t data_size, uint64_t pts90k) {
    if (ctx->on_frame_ex != NULL) {
        ctx->on_frame_ex(media_type, data, data_size, pts90k, ctx->user_data);
    } else if (ctx->on_frame != NULL) {
        ctx->on_frame(media_type, data, data_size, ctx->user_data);
    }
}

static int client_send_request(struct rtsp_client_ctx *ctx, const char *request) {
    return rtsp_send_all(ctx->fd, request, strlen(request));
}

static int client_fill_buffer(struct rtsp_client_ctx *ctx, size_t need) {
    while (ctx->rx_len < need) {
        ssize_t n = recv(ctx->fd, ctx->rx_buf + ctx->rx_len, sizeof(ctx->rx_buf) - ctx->rx_len, 0);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        ctx->rx_len += (size_t)n;
        if (ctx->rx_len == sizeof(ctx->rx_buf) && ctx->rx_len < need) {
            return -1;
        }
    }
    return 0;
}

static void client_consume_buffer(struct rtsp_client_ctx *ctx, size_t n) {
    if (n >= ctx->rx_len) {
        ctx->rx_len = 0;
        return;
    }
    memmove(ctx->rx_buf, ctx->rx_buf + n, ctx->rx_len - n);
    ctx->rx_len -= n;
}

static int client_peek_byte(struct rtsp_client_ctx *ctx, uint8_t *out) {
    if (client_fill_buffer(ctx, 1) < 0) {
        return -1;
    }
    *out = ctx->rx_buf[0];
    return 0;
}

static int client_read_exact(struct rtsp_client_ctx *ctx, void *buf, size_t len) {
    uint8_t *out = (uint8_t *)buf;

    if (client_fill_buffer(ctx, len) < 0) {
        return -1;
    }
    memcpy(out, ctx->rx_buf, len);
    client_consume_buffer(ctx, len);
    return 0;
}

static int client_read_response(struct rtsp_client_ctx *ctx, char *buf, size_t cap) {
    char *headers_end = NULL;
    size_t message_len = 0;
    int content_length = 0;

    if (cap == 0) {
        return -1;
    }

    for (;;) {
        if (ctx->rx_len > 0) {
            ctx->rx_buf[ctx->rx_len] = '\0';
            headers_end = strstr((char *)ctx->rx_buf, "\r\n\r\n");
            if (headers_end != NULL) {
                content_length = rtsp_find_content_length((char *)ctx->rx_buf);
                message_len = (size_t)(headers_end + 4 - (char *)ctx->rx_buf) + (size_t)content_length;
                if (message_len > ctx->rx_len) {
                    if (message_len >= sizeof(ctx->rx_buf)) {
                        return -1;
                    }
                } else {
                    break;
                }
            } else if (ctx->rx_len + 1 >= sizeof(ctx->rx_buf)) {
                return -1;
            }
        }

        if (client_fill_buffer(ctx, ctx->rx_len + 1) < 0) {
            return -1;
        }
    }

    if (message_len + 1 > cap) {
        return -1;
    }
    memcpy(buf, ctx->rx_buf, message_len);
    buf[message_len] = '\0';
    client_consume_buffer(ctx, message_len);
    return 0;
}

static int client_send_simple(struct rtsp_client_ctx *ctx, const char *method, const char *url,
                              const char *extra_headers, char *response, size_t response_size) {
    char req[RTSP_MAX_MESSAGE];
    int cseq = ++ctx->cseq;
    snprintf(req, sizeof(req),
             "%s %s RTSP/1.0\r\n"
             "CSeq: %d\r\n"
             "%s"
             "\r\n",
             method, url, cseq, extra_headers != NULL ? extra_headers : "");
    if (client_send_request(ctx, req) < 0) {
        return -1;
    }
    return client_read_response(ctx, response, response_size);
}

static int decode_base64_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static int decode_base64(const char *src, uint8_t *out, size_t out_cap, size_t *out_size) {
    uint32_t accumulator = 0;
    int bits = 0;
    size_t used = 0;

    for (const char *p = src; *p != '\0'; ++p) {
        int value = 0;
        if (*p == '=') {
            break;
        }
        value = decode_base64_char(*p);
        if (value < 0) {
            return -1;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        while (bits >= 8) {
            bits -= 8;
            if (used >= out_cap) {
                return -1;
            }
            out[used++] = (uint8_t)((accumulator >> bits) & 0xff);
        }
    }

    *out_size = used;
    return used > 0 ? 0 : -1;
}

static int parse_sprop_parameter_sets(struct client_track *track, const char *line, size_t len) {
    const char *key = "sprop-parameter-sets=";
    const char *pos = NULL;
    const char *value = NULL;
    const char *comma = NULL;
    size_t first_len = 0;
    size_t second_len = 0;
    char first[256];
    char second[256];

    pos = strstr(line, key);
    if (pos == NULL || (size_t)(pos - line) >= len) {
        return 0;
    }
    value = pos + strlen(key);
    comma = memchr(value, ',', len - (size_t)(value - line));
    if (comma == NULL) {
        return -1;
    }

    first_len = (size_t)(comma - value);
    if (first_len == 0 || first_len >= sizeof(first)) {
        return -1;
    }

    {
        const char *end = line + len;
        const char *tail = comma + 1;
        while (tail < end && *tail != ';' && *tail != '\r' && *tail != '\n') {
            ++tail;
        }
        second_len = (size_t)(tail - (comma + 1));
    }
    if (second_len == 0 || second_len >= sizeof(second)) {
        return -1;
    }

    memcpy(first, value, first_len);
    first[first_len] = '\0';
    memcpy(second, comma + 1, second_len);
    second[second_len] = '\0';

    if (decode_base64(first, track->sps, sizeof(track->sps), &track->sps_size) < 0 ||
        decode_base64(second, track->pps, sizeof(track->pps), &track->pps_size) < 0) {
        return -1;
    }
    return 0;
}

static int emit_video_codec_config(struct rtsp_client_ctx *ctx, struct client_track *track) {
    if (track->media_type != RTSP_CLIENT_MEDIA_VIDEO || track->codec_config_sent) {
        return 0;
    }
    if (track->sps_size > 0) {
        client_emit_frame(ctx, RTSP_CLIENT_MEDIA_VIDEO, track->sps, track->sps_size, 0);
    }
    if (track->pps_size > 0) {
        client_emit_frame(ctx, RTSP_CLIENT_MEDIA_VIDEO, track->pps, track->pps_size, 0);
    }
    track->codec_config_sent = 1;
    return 0;
}

static int parse_session_id(const char *response, char *out, size_t out_size) {
    char value[128];
    char *semi = NULL;
    if (rtsp_find_header(response, "Session", value, sizeof(value)) < 0) {
        return -1;
    }
    semi = strchr(value, ';');
    if (semi != NULL) {
        *semi = '\0';
    }
    snprintf(out, out_size, "%s", value);
    return 0;
}

static int ensure_fu_capacity(struct client_track *track, size_t needed) {
    if (needed <= track->fu_cap) {
        return 0;
    }
    size_t new_cap = track->fu_cap == 0 ? 4096 : track->fu_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(track->fu_buf, new_cap);
    if (p == NULL) {
        return -1;
    }
    track->fu_buf = p;
    track->fu_cap = new_cap;
    return 0;
}

static int parse_tracks_from_sdp(struct rtsp_client_ctx *ctx, const char *describe_response) {
    char content_base[512];
    const char *body = strstr(describe_response, "\r\n\r\n");
    const char *line = NULL;
    struct client_track *current = NULL;
    int track_count = 0;

    if (body == NULL) {
        return -1;
    }
    body += 4;

    if (rtsp_find_header(describe_response, "Content-Base", content_base, sizeof(content_base)) < 0) {
        snprintf(content_base, sizeof(content_base), "%s", ctx->parsed_url.url);
    }

    /* 这里保持解析逻辑尽量简单，只提取每条 track 的 control URL 和 payload type。 */
    memset(ctx->tracks, 0, sizeof(ctx->tracks));
    line = body;
    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        size_t len = line_end != NULL ? (size_t)(line_end - line) : strlen(line);

        if (len > 2 && strncmp(line, "m=", 2) == 0) {
            char media[32];
            int payload_type = 0;
            if (sscanf(line, "m=%31s 0 RTP/AVP %d", media, &payload_type) == 2) {
                if (strcmp(media, "video") == 0 && track_count < 2) {
                    current = &ctx->tracks[track_count++];
                    current->media_type = RTSP_CLIENT_MEDIA_VIDEO;
                    current->payload_type = (uint8_t)payload_type;
                    current->clock_rate = 90000;
                } else if (strcmp(media, "audio") == 0 && track_count < 2) {
                    current = &ctx->tracks[track_count++];
                    current->media_type = RTSP_CLIENT_MEDIA_AUDIO;
                    current->payload_type = (uint8_t)payload_type;
                    current->clock_rate = 8000;
                } else {
                    current = NULL;
                }
            }
        } else if (current != NULL && len > 10 && strncmp(line, "a=control:", 10) == 0) {
            char value[256];
            if (len - 10 >= sizeof(value)) {
                return -1;
            }
            memcpy(value, line + 10, len - 10);
            value[len - 10] = '\0';
            if (rtsp_build_control_url(content_base, value, current->control_url,
                                       sizeof(current->control_url)) < 0) {
                return -1;
            }
            current->configured = 1;
        } else if (current != NULL && current->media_type == RTSP_CLIENT_MEDIA_VIDEO && len > 7 &&
                   strncmp(line, "a=fmtp:", 7) == 0) {
            if (parse_sprop_parameter_sets(current, line, len) < 0) {
                return -1;
            }
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 2;
    }

    return track_count > 0 ? track_count : -1;
}

static int handle_rtp_payload(struct rtsp_client_ctx *ctx, struct client_track *track,
                              const uint8_t *payload, size_t payload_size, uint64_t pts90k) {
    uint8_t nal_type = 0;
    if (payload_size == 0) {
        return 0;
    }

    if (track->media_type == RTSP_CLIENT_MEDIA_AUDIO) {
        /* PCM 数据一包 RTP 就是完整负载，直接上抛。 */
        client_emit_frame(ctx, RTSP_CLIENT_MEDIA_AUDIO, payload, payload_size, pts90k);
        return 0;
    }

    /* 单 NAL RTP 包可直接回调给上层。 */
    nal_type = (uint8_t)(payload[0] & 0x1f);
    if (nal_type >= 1 && nal_type <= 23) {
        if (emit_video_codec_config(ctx, track) < 0) {
            return -1;
        }
        client_emit_frame(ctx, RTSP_CLIENT_MEDIA_VIDEO, payload, payload_size, pts90k);
        return 0;
    }

    if (nal_type == 28 && payload_size >= 2) {
        /* FU-A 分片需要先重组为完整 H264 NAL，再回调给上层。 */
        uint8_t fu_header = payload[1];
        int start = (fu_header & 0x80) != 0;
        int end = (fu_header & 0x40) != 0;
        uint8_t reconstructed_header = (uint8_t)((payload[0] & 0xe0) | (fu_header & 0x1f));
        const uint8_t *chunk = payload + 2;
        size_t chunk_size = payload_size - 2;

        if (start) {
            track->fu_size = 0;
            if (ensure_fu_capacity(track, chunk_size + 1) < 0) {
                return -1;
            }
            track->fu_buf[track->fu_size++] = reconstructed_header;
        } else if (track->fu_size == 0) {
            return -1;
        } else if (ensure_fu_capacity(track, track->fu_size + chunk_size) < 0) {
            return -1;
        }

        memcpy(track->fu_buf + track->fu_size, chunk, chunk_size);
        track->fu_size += chunk_size;

        if (end) {
            if (emit_video_codec_config(ctx, track) < 0) {
                return -1;
            }
            client_emit_frame(ctx, RTSP_CLIENT_MEDIA_VIDEO, track->fu_buf, track->fu_size, pts90k);
            track->fu_size = 0;
        }
    }

    return 0;
}

static int handle_interleaved_frame(struct rtsp_client_ctx *ctx) {
    uint8_t prefix[4];
    uint8_t *packet = NULL;
    struct client_track *track = NULL;
    size_t payload_offset = 12;
    uint16_t size = 0;
    uint8_t cc = 0;
    int extension = 0;
    uint32_t rtp_timestamp = 0;
    uint64_t pts90k = 0;

    /* RTSP over TCP 的复用帧以 '$' + 通道号 + 16 位长度开头。 */
    if (client_read_exact(ctx, prefix, sizeof(prefix)) < 0) {
        return -1;
    }
    size = (uint16_t)((prefix[2] << 8) | prefix[3]);
    packet = (uint8_t *)malloc(size);
    if (packet == NULL) {
        return -1;
    }
    if (client_read_exact(ctx, packet, size) < 0) {
        free(packet);
        return -1;
    }

    if (size < 12) {
        free(packet);
        return 0;
    }

    for (size_t i = 0; i < sizeof(ctx->tracks) / sizeof(ctx->tracks[0]); ++i) {
        if (ctx->tracks[i].configured && prefix[1] == ctx->tracks[i].rtp_channel) {
            track = &ctx->tracks[i];
            break;
        }
        if (ctx->tracks[i].configured && prefix[1] == ctx->tracks[i].rtcp_channel) {
            /* 这里暂时忽略 RTCP，只处理 RTP 负载分发。 */
            free(packet);
            return 0;
        }
    }
    if (track == NULL) {
        free(packet);
        return 0;
    }

    /* 跳过 RTP 的 CSRC 和扩展头，定位真正的媒体负载。 */
    rtp_timestamp = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) |
                    (uint32_t)packet[7];
    cc = (uint8_t)(packet[0] & 0x0f);
    extension = (packet[0] & 0x10) != 0;
    payload_offset += (size_t)cc * 4;
    if (extension) {
        if (payload_offset + 4 > size) {
            free(packet);
            return -1;
        }
        uint16_t ext_len = (uint16_t)((packet[payload_offset + 2] << 8) | packet[payload_offset + 3]);
        payload_offset += 4 + (size_t)ext_len * 4;
    }
    if (payload_offset > size) {
        free(packet);
        return -1;
    }

    if (track->clock_rate == 0) {
        track->clock_rate = track->media_type == RTSP_CLIENT_MEDIA_AUDIO ? 8000 : 90000;
    }
    if (!track->first_rtp_timestamp_set) {
        track->first_rtp_timestamp = rtp_timestamp;
        track->first_rtp_timestamp_set = 1;
    }
    pts90k = ((uint64_t)((uint32_t)(rtp_timestamp - track->first_rtp_timestamp)) * 90000ULL) /
             (uint64_t)track->clock_rate;

    if (handle_rtp_payload(ctx, track, packet + payload_offset, size - payload_offset, pts90k) < 0) {
        free(packet);
        return -1;
    }

    free(packet);
    return 0;
}

static int receive_stream_loop(struct rtsp_client_ctx *ctx, const volatile sig_atomic_t *running) {
    while (running == NULL || *running) {
        uint8_t ch = 0;
        if (client_peek_byte(ctx, &ch) < 0) {
            printf("recv error: %s\n", strerror(errno));
            return -1;
        }
        if (ch == '$') {
            if (handle_interleaved_frame(ctx) < 0) {
                printf("handle interleaved frame fail\n");
                return -1;
            }
        } else {
            /* 如果服务端发来异步 RTSP 响应，这里把它消费掉。 */
            char response[RTSP_MAX_MESSAGE];
            if (client_read_response(ctx, response, sizeof(response)) < 0) {
                printf("read response fail, first byte=0x%02x errno=%s\n", ch, strerror(errno));
                return -1;
            }
        }
    }
    return 0;
}

int rtsp_client_run(const struct rtsp_client_config *config) {
    struct rtsp_client_ctx ctx;
    char response[RTSP_MAX_MESSAGE];
    char headers[512];
    int track_count = 0;
    int rc = -1;

    if (config == NULL || config->url == NULL || (config->on_frame == NULL && config->on_frame_ex == NULL)) {
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;
    ctx.on_frame = config->on_frame;
    ctx.on_frame_ex = config->on_frame_ex;
    ctx.user_data = config->user_data;

    if (rtsp_parse_url(config->url, &ctx.parsed_url) < 0) {
        goto done;
    }

    ctx.fd = rtsp_tcp_connect(ctx.parsed_url.host, ctx.parsed_url.port);
    if (ctx.fd < 0) {
        goto done;
    }

    if (client_send_simple(&ctx, "OPTIONS", ctx.parsed_url.url, NULL, response, sizeof(response)) < 0) {
        goto done;
    }
    if (client_send_simple(&ctx, "DESCRIBE", ctx.parsed_url.url, "Accept: application/sdp\r\n", response,
                           sizeof(response)) < 0) {
        goto done;
    }
    track_count = parse_tracks_from_sdp(&ctx, response);
    if (track_count < 0) {
        goto done;
    }

    /* 每条发现到的 track 都分配独立的 interleaved 通道对。 */
    for (size_t i = 0; i < sizeof(ctx.tracks) / sizeof(ctx.tracks[0]); ++i) {
        struct client_track *track = &ctx.tracks[i];
        if (!track->configured) {
            continue;
        }

        track->rtp_channel = (uint8_t)(i * 2);
        track->rtcp_channel = (uint8_t)(i * 2 + 1);
        if (ctx.session_id[0] == '\0') {
            snprintf(headers, sizeof(headers), "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                     track->rtp_channel, track->rtcp_channel);
        } else {
            snprintf(headers, sizeof(headers),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n"
                     "Session: %s\r\n",
                     track->rtp_channel, track->rtcp_channel, ctx.session_id);
        }
        if (client_send_simple(&ctx, "SETUP", track->control_url, headers, response, sizeof(response)) < 0) {
            goto done;
        }
        if (ctx.session_id[0] == '\0' &&
            parse_session_id(response, ctx.session_id, sizeof(ctx.session_id)) < 0) {
            goto done;
        }
    }

    snprintf(headers, sizeof(headers), "Session: %s\r\n", ctx.session_id);
    if (client_send_simple(&ctx, "PLAY", ctx.parsed_url.url, headers, response, sizeof(response)) < 0) {
        goto done;
    }

    rc = receive_stream_loop(&ctx, config->running);

done:
    if (ctx.fd >= 0 && ctx.session_id[0] != '\0') {
        snprintf(headers, sizeof(headers), "Session: %s\r\n", ctx.session_id);
        client_send_simple(&ctx, "TEARDOWN", ctx.parsed_url.url, headers, response, sizeof(response));
    }
    if (ctx.fd >= 0) {
        close(ctx.fd);
    }
    for (size_t i = 0; i < sizeof(ctx.tracks) / sizeof(ctx.tracks[0]); ++i) {
        free(ctx.tracks[i].fu_buf);
    }
    return rc;
}
