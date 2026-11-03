// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "zigbee_interview_utils.h"
#include <cstring>

namespace {

constexpr uint16_t BASIC_CLUSTER_ID = 0x0000;

static bool endpoint_has_basic_cluster(const ZapDevice& dev, uint8_t ep_index) {
    for (uint8_t i = 0; i < ZAP_CLUSTERS_PER_EP; i++) {
        if (dev.clusters[ep_index][i] == BASIC_CLUSTER_ID) return true;
    }
    return false;
}

static bool append_unique(uint8_t ep, uint8_t* out_eps, uint8_t& out_count, uint8_t out_cap) {
    if (ep == 0 || out_count >= out_cap) return false;
    for (uint8_t i = 0; i < out_count; i++) {
        if (out_eps[i] == ep) return false;
    }
    out_eps[out_count++] = ep;
    return true;
}

}  // namespace

uint8_t zigbee_interview_build_basic_probe_order(const ZapDevice& dev,
                                                 uint8_t* out_eps,
                                                 uint8_t out_cap) {
    if (!out_eps || out_cap == 0) return 0;

    uint8_t out_count = 0;
    const uint8_t ep_limit = (dev.endpoint_count < 8) ? dev.endpoint_count : 8;

    for (uint8_t i = 0; i < ep_limit; i++) {
        if (endpoint_has_basic_cluster(dev, i)) {
            append_unique(dev.endpoints[i], out_eps, out_count, out_cap);
        }
    }

    for (uint8_t i = 0; i < ep_limit; i++) {
        append_unique(dev.endpoints[i], out_eps, out_count, out_cap);
    }

    return out_count;
}

// ── ZCL Basic-cluster identity parser ────────────────────────────────────
// Used both by the synchronous interview and by the late-identity path
// that enriches devices from unsolicited reports / delayed responses.

// ZCL data-type size table (partial — only what Basic carries).
static int zcl_value_size(uint8_t type, const uint8_t* p, uint8_t remaining) {
    switch (type) {
        case 0x10: return 1;                               // bool
        case 0x18: case 0x20: case 0x28: case 0x30: return 1;
        case 0x19: case 0x21: case 0x29: case 0x31: return 2;
        case 0x1B: case 0x23: case 0x2B: case 0x39: return 4;
        case 0x42: case 0x41:                              // char / octet string
            return (remaining > 0) ? (1 + p[0]) : -1;
        case 0x44: case 0x43:                              // long char / long octet
            if (remaining < 2) return -1;
            return 2 + (int)(p[0] | (p[1] << 8));
    }
    return -1;                                             // unknown
}

// Length of the valid UTF-8 sequence at s (1..4 bytes, RFC 3629: no overlong
// forms, no surrogates, nothing past U+10FFFF), 0 when invalid or cut short.
static size_t utf8_len(const uint8_t* s, size_t avail) {
    const uint8_t c = s[0];
    if (c < 0x80) return 1;
    size_t n;
    uint8_t lo = 0x80, hi = 0xBF;                          // first continuation byte
    if (c >= 0xC2 && c <= 0xDF)      n = 1;
    else if (c == 0xE0)            { n = 2; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) n = 2;
    else if (c == 0xED)            { n = 2; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) n = 2;
    else if (c == 0xF0)            { n = 3; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 3;
    else if (c == 0xF4)            { n = 3; hi = 0x8F; }
    else return 0;
    if (avail < n + 1) return 0;
    for (size_t k = 1; k <= n; k++) {
        if (s[k] < (k == 1 ? lo : 0x80) || s[k] > (k == 1 ? hi : 0xBF)) return 0;
    }
    return n + 1;
}

size_t zigbee_copy_text(char* dst, size_t cap, const uint8_t* src, size_t len) {
    if (!dst || cap == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; src && i < len && src[i];) {
        size_t n = utf8_len(src + i, len - i);
        if (n == 1 && (src[i] < 0x20 || src[i] == 0x7F)) n = 0;   // control byte
        if (o + (n ? n : 1) > cap - 1) break;                    // whole characters only
        if (n) {
            for (size_t k = 0; k < n; k++) dst[o + k] = static_cast<char>(src[i + k]);   // dst may be src
            o += n;
            i += n;
        } else {
            dst[o++] = '?';
            i++;
        }
    }
    dst[o] = '\0';
    return o;
}

bool zigbee_parse_basic_identity(const uint8_t* data, uint8_t data_len,
                                 char* model_out, uint8_t model_cap,
                                 char* mfg_out, uint8_t mfg_cap,
                                 uint16_t* mfg_code_out) {
    if (!data || data_len < 3) return false;

    // ZCL header: [frame_ctrl, seq, cmd_id] (plus optional 2-byte mfg code when
    // frame_ctrl bit2 is set; Basic responses we care about are global/no-mfg).
    const uint8_t frame_ctrl = data[0];
    uint8_t pos = (frame_ctrl & 0x04) ? 5 : 3;     // skip mfg code when present
    if (pos > data_len) return false;
    const uint8_t cmd_id = data[(frame_ctrl & 0x04) ? 4 : 2];

    // Accept only Read Attributes Response (0x01) or Report Attributes (0x0A).
    const bool is_read_rsp = (cmd_id == 0x01);
    const bool is_report   = (cmd_id == 0x0A);
    if (!is_read_rsp && !is_report) return false;

    bool any = false;
    while (pos + 3 <= data_len) {
        uint16_t attr = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        if (is_read_rsp) {
            uint8_t status = data[pos++];
            if (status != 0x00) continue;          // attribute read failed
        }
        if (pos >= data_len) break;
        uint8_t type = data[pos++];
        int val_sz = zcl_value_size(type, data + pos, (uint8_t)(data_len - pos));
        if (val_sz < 0 || pos + val_sz > data_len) return any;

        if (type == 0x42 && val_sz >= 1) {
            uint8_t str_len = data[pos];
            if (str_len != 0xFF && pos + 1 + str_len <= data_len) {
                if (attr == 0x0005) { zigbee_copy_text(model_out, model_cap, data + pos + 1, str_len); any = true; }
                if (attr == 0x0004) { zigbee_copy_text(mfg_out,   mfg_cap,   data + pos + 1, str_len); any = true; }
            }
        } else if (attr == 0x0001 && type == 0x21 && val_sz == 2 && mfg_code_out) {
            *mfg_code_out = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
            any = true;
        }
        pos += (uint8_t)val_sz;
    }
    return any;
}
