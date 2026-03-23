#include "rtsp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "rtsp_common.h"

#define STREAM_FPS 25
#define RTP_PAYLOAD_TYPE_AAC 97

enum media_track_type {
    MEDIA_TRACK_VIDEO = 0,
    MEDIA_TRACK_AUDIO = 1,
};

struct transport_info {
    int use_tcp;
    int client_rtp_port;
    int client_rtcp_port;
    int interleaved_rtp_channel;
    int interleaved_rtcp_channel;
};

struct media_track {
    int configured;
    int rtp_fd;
    int rtcp_fd;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t payload_type;
    struct transport_info transport;
};

struct server_session {
    int rtsp_fd;
    int sender_running;
    int stop_sender;
    uint32_t session_id;
    struct sockaddr_in peer_addr;
    struct media_track video_track;
    struct media_track audio_track;
    const struct rtsp_h264_stream_source *stream;
    pthread_t sender_thread;
    pthread_mutex_t write_lock;
    size_t audio_frame_index;
    size_t frame_index;
    uint64_t audio_packet_accumulator;
};

static int open_udp_socket(uint16_t *bound_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    *bound_port = ntohs(addr.sin_port);
    return fd;
}

static int parse_transport_header(const char *req, struct transport_info *transport) {
    char value[512];
    char *pos = NULL;

    memset(transport, 0, sizeof(*transport));
    transport->client_rtp_port = -1;
    transport->client_rtcp_port = -1;
    transport->interleaved_rtp_channel = 0;
    transport->interleaved_rtcp_channel = 1;

    if (rtsp_find_header(req, "Transport", value, sizeof(value)) < 0) {
        return -1;
    }

    /* 同时支持 RTP over RTSP(TCP 复用) 和普通 UDP 传输。 */
    if (strstr(value, "RTP/AVP/TCP") != NULL) {
        transport->use_tcp = 1;
        pos = strcasestr(value, "interleaved=");
        if (pos != NULL) {
            sscanf(pos, "interleaved=%d-%d", &transport->interleaved_rtp_channel,
                   &transport->interleaved_rtcp_channel);
        }
        return 0;
    }

    pos = strcasestr(value, "client_port=");
    if (pos == NULL) {
        return -1;
    }
    if (sscanf(pos, "client_port=%d-%d", &transport->client_rtp_port,
               &transport->client_rtcp_port) < 1) {
        return -1;
    }
    if (transport->client_rtcp_port < 0) {
        transport->client_rtcp_port = transport->client_rtp_port + 1;
    }
    return 0;
}

static int session_send_bytes(struct server_session *session, const void *buf, size_t len) {
    int rc;
    /* RTSP 控制响应和 TCP 复用的 RTP 数据共用同一条连接。 */
    pthread_mutex_lock(&session->write_lock);
    rc = rtsp_send_all(session->rtsp_fd, buf, len);
    pthread_mutex_unlock(&session->write_lock);
    return rc;
}

static int session_send_response(struct server_session *session, const char *response) {
    return session_send_bytes(session, response, strlen(response));
}

static int send_rtp_packet(struct server_session *session, struct media_track *track,
                           const uint8_t *payload, size_t payload_size, int marker) {
    uint8_t packet[1600];
    uint16_t seq = track->seq++;
    size_t total = 12 + payload_size;

    if (total > sizeof(packet)) {
        return -1;
    }

    /* 按 track 的序号和时间戳状态组装最小 RTP 头。 */
    packet[0] = 0x80;
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | track->payload_type);
    packet[2] = (uint8_t)(seq >> 8);
    packet[3] = (uint8_t)(seq & 0xff);
    packet[4] = (uint8_t)(track->timestamp >> 24);
    packet[5] = (uint8_t)(track->timestamp >> 16);
    packet[6] = (uint8_t)(track->timestamp >> 8);
    packet[7] = (uint8_t)(track->timestamp & 0xff);
    packet[8] = (uint8_t)(track->ssrc >> 24);
    packet[9] = (uint8_t)(track->ssrc >> 16);
    packet[10] = (uint8_t)(track->ssrc >> 8);
    packet[11] = (uint8_t)(track->ssrc & 0xff);
    memcpy(packet + 12, payload, payload_size);

    if (track->transport.use_tcp) {
        /* RTSP TCP 复用帧格式：'$' + 通道号 + 16 位长度。 */
        uint8_t prefix[4];
        prefix[0] = '$';
        prefix[1] = (uint8_t)track->transport.interleaved_rtp_channel;
        prefix[2] = (uint8_t)(total >> 8);
        prefix[3] = (uint8_t)(total & 0xff);

        pthread_mutex_lock(&session->write_lock);
        int rc = rtsp_send_all(session->rtsp_fd, prefix, sizeof(prefix));
        if (rc == 0) {
            rc = rtsp_send_all(session->rtsp_fd, packet, total);
        }
        pthread_mutex_unlock(&session->write_lock);
        return rc;
    }

    struct sockaddr_in dst = session->peer_addr;
    dst.sin_port = htons((uint16_t)track->transport.client_rtp_port);
    return sendto(track->rtp_fd, packet, total, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0 ? -1 : 0;
}

static int send_h264_nal(struct server_session *session, const uint8_t *nal, size_t nal_size, int marker) {
    struct media_track *track = &session->video_track;
    if (nal_size <= RTP_MTU_PAYLOAD) {
        return send_rtp_packet(session, track, nal, nal_size, marker);
    }

    /* 超过 MTU 的 NAL 要切成 FU-A 分片发送。 */
    uint8_t fua[RTP_MTU_PAYLOAD + 2];
    uint8_t nal_header = nal[0];
    uint8_t fu_indicator = (uint8_t)((nal_header & 0xe0) | 28);
    uint8_t nal_type = (uint8_t)(nal_header & 0x1f);
    size_t offset = 1;

    while (offset < nal_size) {
        size_t chunk = nal_size - offset;
        int start = offset == 1;
        int end = 0;

        if (chunk > RTP_MTU_PAYLOAD - 2) {
            chunk = RTP_MTU_PAYLOAD - 2;
        }
        end = offset + chunk == nal_size;

        fua[0] = fu_indicator;
        fua[1] = (uint8_t)((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type);
        memcpy(fua + 2, nal + offset, chunk);

        if (send_rtp_packet(session, track, fua, chunk + 2, marker && end) < 0) {
            return -1;
        }
        offset += chunk;
    }
    return 0;
}

static int fill_audio_payload(struct server_session *session, uint8_t *payload, size_t payload_size) {
    const struct rtsp_aac_frame *frame = NULL;
    uint16_t au_headers_bits = 16;
    uint16_t au_size = 0;

    if (session->stream->audio_frames == NULL || session->stream->audio_frame_count == 0) {
        (void)payload;
        (void)payload_size;
        return -1;
    }

    frame = &session->stream->audio_frames[session->audio_frame_index];
    au_size = (uint16_t)frame->size;
    if (frame->size > 0x1fff || payload_size < frame->size + 4) {
        return -1;
    }

    payload[0] = (uint8_t)(au_headers_bits >> 8);
    payload[1] = (uint8_t)(au_headers_bits & 0xff);
    payload[2] = (uint8_t)(au_size >> 5);
    payload[3] = (uint8_t)((au_size & 0x1f) << 3);
    memcpy(payload + 4, frame->data, frame->size);

    session->audio_frame_index = (session->audio_frame_index + 1) % session->stream->audio_frame_count;
    return (int)(frame->size + 4);
}

static void *sender_thread_main(void *arg) {
    struct server_session *session = (struct server_session *)arg;
    struct rtsp_h264_nal_unit nals[64];
    uint32_t fps_num = session->stream->video_fps_num > 0 ? session->stream->video_fps_num : STREAM_FPS;
    uint32_t fps_den = session->stream->video_fps_den > 0 ? session->stream->video_fps_den : 1;
    uint32_t audio_sample_rate = session->stream->audio_sample_rate;
    uint32_t audio_samples_per_frame = session->stream->audio_samples_per_frame;
    useconds_t frame_sleep_us = (useconds_t)(((uint64_t)1000000 * fps_den) / fps_num);
    uint32_t frame_ts_step = (uint32_t)(((uint64_t)RTP_CLOCK_H264 * fps_den) / fps_num);
    uint64_t audio_packet_threshold = (uint64_t)fps_num * audio_samples_per_frame;
    uint8_t audio_payload[1600];

    while (!session->stop_sender) {
        if (session->video_track.configured && session->stream->frame_count > 0) {
            const struct rtsp_h264_frame *frame = &session->stream->frames[session->frame_index];
            size_t nal_count = rtsp_h264_parse_nals_from_buffer(frame->data, frame->size, nals, 64);

            for (size_t i = 0; i < nal_count; ++i) {
                if (session->stop_sender) {
                    break;
                }
                if (send_h264_nal(session, nals[i].data, nals[i].size, i + 1 == nal_count) < 0) {
                    session->stop_sender = 1;
                    break;
                }
            }
            session->video_track.timestamp += frame_ts_step;
            session->frame_index = (session->frame_index + 1) % session->stream->frame_count;
        }

        if (session->audio_track.configured && session->stream->has_audio_track) {
            session->audio_packet_accumulator += (uint64_t)audio_sample_rate * fps_den;
            while (session->audio_packet_accumulator >= audio_packet_threshold && !session->stop_sender) {
                int audio_payload_size = fill_audio_payload(session, audio_payload, sizeof(audio_payload));
                if (audio_payload_size < 0) {
                    session->stop_sender = 1;
                    break;
                }
                if (send_rtp_packet(session, &session->audio_track, audio_payload,
                                    (size_t)audio_payload_size, 1) < 0) {
                    session->stop_sender = 1;
                    break;
                }
                session->audio_track.timestamp += audio_samples_per_frame;
                session->audio_packet_accumulator -= audio_packet_threshold;
            }
        }

        usleep(frame_sleep_us);
    }

    session->sender_running = 0;
    return NULL;
}

static void stop_sender(struct server_session *session) {
    if (!session->sender_running) {
        return;
    }
    session->stop_sender = 1;
    pthread_join(session->sender_thread, NULL);
    session->sender_running = 0;
}

static int start_sender(struct server_session *session) {
    if (session->sender_running) {
        return 0;
    }
    session->stop_sender = 0;
    session->sender_running = 1;
    if (pthread_create(&session->sender_thread, NULL, sender_thread_main, session) != 0) {
        session->sender_running = 0;
        return -1;
    }
    return 0;
}

static int handle_options(struct server_session *session, int cseq) {
    char resp[RTSP_MAX_MESSAGE];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
             "\r\n",
             cseq);
    return session_send_response(session, resp);
}

static int handle_describe(struct server_session *session, int cseq, const char *url) {
    char sdp[1024];
    char resp[RTSP_MAX_MESSAGE];
    int len = snprintf(sdp, sizeof(sdp),
                       "v=0\r\n"
                       "o=- 0 0 IN IP4 0.0.0.0\r\n"
                       "s=%s\r\n"
                       "t=0 0\r\n"
                       "a=control:*\r\n"
                       "m=video 0 RTP/AVP 96\r\n"
                       "c=IN IP4 0.0.0.0\r\n"
                       "a=rtpmap:96 H264/90000\r\n"
                       "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s\r\n"
                       "a=control:trackID=0\r\n",
                       session->stream->stream_name, session->stream->profile_level_id,
                       session->stream->sprop_parameter_sets);

    if (len < 0 || (size_t)len >= sizeof(sdp)) {
        return -1;
    }
    if (session->stream->has_audio_track) {
        uint32_t audio_sample_rate = session->stream->audio_sample_rate;
        uint32_t audio_channels = session->stream->audio_channels;
        len += snprintf(sdp + len, sizeof(sdp) - (size_t)len,
                        "m=audio 0 RTP/AVP 97\r\n"
                        "c=IN IP4 0.0.0.0\r\n"
                        "a=rtpmap:97 MPEG4-GENERIC/%u/%u\r\n"
                        "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;config=%s;"
                        "SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n"
                        "a=control:trackID=1\r\n"
                        "a=ptime:23\r\n",
                        audio_sample_rate, audio_channels, session->stream->audio_config_hex);
        if (len < 0 || (size_t)len >= sizeof(sdp)) {
            return -1;
        }
    }

    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Content-Base: %s/\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             cseq, url, strlen(sdp), sdp);
    return session_send_response(session, resp);
}

static int setup_track_transport(struct server_session *session, struct media_track *track,
                                 const struct transport_info *transport, int cseq) {
    char resp[RTSP_MAX_MESSAGE];

    if (track->rtp_fd >= 0) {
        close(track->rtp_fd);
        track->rtp_fd = -1;
    }
    if (track->rtcp_fd >= 0) {
        close(track->rtcp_fd);
        track->rtcp_fd = -1;
    }

    track->transport = *transport;
    track->configured = 1;

    if (!transport->use_tcp) {
        /* UDP 模式下，每个 track 都单独分配一对 RTP/RTCP 端口。 */
        uint16_t server_rtp_port = 0;
        uint16_t server_rtcp_port = 0;

        track->rtp_fd = open_udp_socket(&server_rtp_port);
        track->rtcp_fd = open_udp_socket(&server_rtcp_port);
        if (track->rtp_fd < 0 || track->rtcp_fd < 0) {
            return -1;
        }

        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u\r\n"
                 "Session: %u\r\n"
                 "\r\n",
                 cseq, transport->client_rtp_port, transport->client_rtcp_port, server_rtp_port,
                 server_rtcp_port, session->session_id);
        return session_send_response(session, resp);
    }

    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
             "Session: %u\r\n"
             "\r\n",
             cseq, transport->interleaved_rtp_channel, transport->interleaved_rtcp_channel,
             session->session_id);
    return session_send_response(session, resp);
}

static int handle_setup(struct server_session *session, int cseq, const char *req, const char *url) {
    struct transport_info transport;
    struct media_track *track = NULL;
    int is_audio_track = strstr(url, "trackID=1") != NULL;

    if (parse_transport_header(req, &transport) < 0) {
        char resp[RTSP_MAX_MESSAGE];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 461 Unsupported Transport\r\n"
                 "CSeq: %d\r\n"
                 "\r\n",
                 cseq);
        return session_send_response(session, resp);
    }

    stop_sender(session);
    if (is_audio_track && !session->stream->has_audio_track) {
        char resp[RTSP_MAX_MESSAGE];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 404 Not Found\r\n"
                 "CSeq: %d\r\n"
                 "\r\n",
                 cseq);
        return session_send_response(session, resp);
    }
    /* trackID=0 表示视频，trackID=1 表示音频。 */
    track = is_audio_track ? &session->audio_track : &session->video_track;
    return setup_track_transport(session, track, &transport, cseq);
}

static int handle_play(struct server_session *session, int cseq, const char *url) {
    char resp[RTSP_MAX_MESSAGE];
    if (start_sender(session) < 0) {
        return -1;
    }
    /* 在 RTP-Info 中返回两条 track 的初始 RTP 状态。 */
    if (session->stream->has_audio_track) {
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %u\r\n"
                 "Range: npt=0.000-\r\n"
                 "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u,url=%s/trackID=1;seq=%u;rtptime=%u\r\n"
                 "\r\n",
                 cseq, session->session_id, url, session->video_track.seq,
                 session->video_track.timestamp, url, session->audio_track.seq,
                 session->audio_track.timestamp);
    } else {
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %u\r\n"
                 "Range: npt=0.000-\r\n"
                 "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u\r\n"
                 "\r\n",
                 cseq, session->session_id, url, session->video_track.seq,
                 session->video_track.timestamp);
    }
    return session_send_response(session, resp);
}

static int handle_pause(struct server_session *session, int cseq) {
    char resp[RTSP_MAX_MESSAGE];
    stop_sender(session);
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %u\r\n"
             "\r\n",
             cseq, session->session_id);
    return session_send_response(session, resp);
}

static int handle_teardown(struct server_session *session, int cseq) {
    char resp[RTSP_MAX_MESSAGE];
    stop_sender(session);
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %u\r\n"
             "\r\n",
             cseq, session->session_id);
    return session_send_response(session, resp);
}

static void close_session(struct server_session *session) {
    stop_sender(session);
    if (session->video_track.rtp_fd >= 0) {
        close(session->video_track.rtp_fd);
    }
    if (session->video_track.rtcp_fd >= 0) {
        close(session->video_track.rtcp_fd);
    }
    if (session->audio_track.rtp_fd >= 0) {
        close(session->audio_track.rtp_fd);
    }
    if (session->audio_track.rtcp_fd >= 0) {
        close(session->audio_track.rtcp_fd);
    }
    if (session->rtsp_fd >= 0) {
        close(session->rtsp_fd);
    }
    pthread_mutex_destroy(&session->write_lock);
}

static void *session_thread_main(void *arg) {
    struct server_session *session = (struct server_session *)arg;
    char req[RTSP_MAX_MESSAGE];
    char url[512];
    size_t req_len = 0;

    while (rtsp_read_message(session->rtsp_fd, req, sizeof(req), &req_len) == 0) {
        int cseq = 1;
        char cseq_str[32];

        (void)req_len;
        if (rtsp_find_header(req, "CSeq", cseq_str, sizeof(cseq_str)) == 0) {
            cseq = atoi(cseq_str);
        }
        rtsp_extract_request_url(req, url, sizeof(url));

        if (strncmp(req, "OPTIONS", 7) == 0) {
            if (handle_options(session, cseq) < 0) {
                break;
            }
        } else if (strncmp(req, "DESCRIBE", 8) == 0) {
            if (handle_describe(session, cseq, url) < 0) {
                break;
            }
        } else if (strncmp(req, "SETUP", 5) == 0) {
            if (handle_setup(session, cseq, req, url) < 0) {
                break;
            }
        } else if (strncmp(req, "PLAY", 4) == 0) {
            if (handle_play(session, cseq, url) < 0) {
                break;
            }
        } else if (strncmp(req, "PAUSE", 5) == 0) {
            if (handle_pause(session, cseq) < 0) {
                break;
            }
        } else if (strncmp(req, "TEARDOWN", 8) == 0) {
            handle_teardown(session, cseq);
            break;
        } else {
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "RTSP/1.0 405 Method Not Allowed\r\n"
                     "CSeq: %d\r\n"
                     "\r\n",
                     cseq);
            if (session_send_response(session, resp) < 0) {
                break;
            }
        }
    }

    close_session(session);
    free(session);
    return NULL;
}

int rtsp_server_run(const struct rtsp_h264_stream_source *stream, uint16_t port,
                    const volatile sig_atomic_t *running) {
    int listen_fd = -1;
    int reuse = 1;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 16) < 0) {
        close(listen_fd);
        return -1;
    }

    printf("RTSP server listening on rtsp://0.0.0.0:%u/live\n", port);
    printf("Source: %s\n", stream->stream_name);
    fflush(stdout);

    while (running == NULL || *running) {
        fd_set rfds;
        struct timeval tv;
        int ready = 0;

        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(listen_fd);
            return -1;
        }
        if (ready == 0 || !FD_ISSET(listen_fd, &rfds)) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(listen_fd);
            return -1;
        }

        /* 每个 TCP 客户端都拥有独立的 RTSP 会话和发送线程。 */
        struct server_session *session = (struct server_session *)calloc(1, sizeof(*session));
        if (session == NULL) {
            close(client_fd);
            continue;
        }

        session->rtsp_fd = client_fd;
        session->session_id = (uint32_t)(rand() & 0x7fffffff);
        session->peer_addr = peer;
        session->stream = stream;
        session->video_track.rtp_fd = -1;
        session->video_track.rtcp_fd = -1;
        session->video_track.payload_type = RTP_PAYLOAD_TYPE_H264;
        session->video_track.seq = (uint16_t)(rand() & 0xffff);
        session->video_track.timestamp = (uint32_t)rand();
        session->video_track.ssrc = (uint32_t)rand();
        session->audio_track.rtp_fd = -1;
        session->audio_track.rtcp_fd = -1;
        session->audio_track.payload_type = RTP_PAYLOAD_TYPE_AAC;
        session->audio_track.seq = (uint16_t)(rand() & 0xffff);
        session->audio_track.timestamp = (uint32_t)rand();
        session->audio_track.ssrc = (uint32_t)rand();
        pthread_mutex_init(&session->write_lock, NULL);

        pthread_t tid;
        if (pthread_create(&tid, NULL, session_thread_main, session) != 0) {
            close_session(session);
            free(session);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
