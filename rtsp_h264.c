#include "rtsp_h264.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "default_h264_1024x768.h"

static int starts_with(const uint8_t *data, size_t size, size_t i, size_t *prefix_len) {
    if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        *prefix_len = 4;
        return 1;
    }
    if (i + 3 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
        *prefix_len = 3;
        return 1;
    }
    return 0;
}

static int base64_encode(const uint8_t *data, size_t size, char *out, size_t out_size) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t j = 0;

    while (i < size) {
        size_t remain = size - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = remain > 1 ? data[i++] : 0;
        uint32_t octet_c = remain > 2 ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        if (j + 4 >= out_size) {
            return -1;
        }
        out[j++] = tbl[(triple >> 18) & 0x3f];
        out[j++] = tbl[(triple >> 12) & 0x3f];
        out[j++] = remain > 1 ? tbl[(triple >> 6) & 0x3f] : '=';
        out[j++] = remain > 2 ? tbl[triple & 0x3f] : '=';
    }

    out[j] = '\0';
    return 0;
}

static int configure_stream_source(struct rtsp_h264_stream_source *stream, const uint8_t *data, size_t size,
                                   const char *name) {
    struct rtsp_h264_nal_unit nals[64];
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_size = 0;
    size_t pps_size = 0;
    char sps_b64[128];
    char pps_b64[64];
    size_t nal_count = 0;

    memset(stream, 0, sizeof(*stream));

    /* 先提取 SPS/PPS，供 DESCRIBE 返回合法的 H264 fmtp 参数。 */
    nal_count = rtsp_h264_parse_nals_from_buffer(data, size, nals, 64);
    if (nal_count == 0) {
        return -1;
    }

    for (size_t i = 0; i < nal_count; ++i) {
        uint8_t nal_type = (uint8_t)(nals[i].data[0] & 0x1f);
        if (nal_type == 7 && sps == NULL) {
            sps = nals[i].data;
            sps_size = nals[i].size;
        } else if (nal_type == 8 && pps == NULL) {
            pps = nals[i].data;
            pps_size = nals[i].size;
        }
    }

    if (sps == NULL || pps == NULL || sps_size < 4) {
        return -1;
    }
    if (base64_encode(sps, sps_size, sps_b64, sizeof(sps_b64)) < 0 ||
        base64_encode(pps, pps_size, pps_b64, sizeof(pps_b64)) < 0) {
        return -1;
    }

    stream->data = data;
    stream->size = size;
    snprintf(stream->sprop_parameter_sets, sizeof(stream->sprop_parameter_sets), "%s,%s", sps_b64,
             pps_b64);
    snprintf(stream->profile_level_id, sizeof(stream->profile_level_id), "%02X%02X%02X", sps[1], sps[2],
             sps[3]);
    snprintf(stream->stream_name, sizeof(stream->stream_name), "%s", name);
    return 0;
}

int rtsp_h264_stream_source_init_default(struct rtsp_h264_stream_source *stream) {
    return configure_stream_source(stream, _tmp_default_1024x768_h264, _tmp_default_1024x768_h264_len,
                                   "Embedded 1024x768 Test Pattern");
}

int rtsp_h264_stream_source_load_file(struct rtsp_h264_stream_source *stream, const char *path) {
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    long file_size = 0;

    memset(stream, 0, sizeof(*stream));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    /* 载入的 .h264 文件会常驻内存，供服务端循环发送。 */
    buf = (uint8_t *)malloc((size_t)file_size);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (configure_stream_source(stream, buf, (size_t)file_size, path) < 0) {
        free(buf);
        return -1;
    }
    stream->owned_data = buf;
    return 0;
}

void rtsp_h264_stream_source_cleanup(struct rtsp_h264_stream_source *stream) {
    free(stream->owned_data);
    memset(stream, 0, sizeof(*stream));
}

size_t rtsp_h264_parse_nals_from_buffer(const uint8_t *data, size_t size,
                                        struct rtsp_h264_nal_unit *nals, size_t cap) {
    size_t count = 0;
    size_t i = 0;

    /* 扫描 Annex-B 起始码，返回去掉起始码后的 NAL 数据范围。 */
    while (i + 3 < size) {
        size_t prefix = 0;
        if (!starts_with(data, size, i, &prefix)) {
            ++i;
            continue;
        }

        size_t nal_start = i + prefix;
        size_t j = nal_start;
        while (j + 3 < size) {
            size_t next_prefix = 0;
            if (starts_with(data, size, j, &next_prefix)) {
                break;
            }
            ++j;
        }
        if (j + 3 >= size) {
            j = size;
        }
        if (count < cap) {
            nals[count].data = &data[nal_start];
            nals[count].size = j - nal_start;
            ++count;
        }
        i = j;
    }

    return count;
}
