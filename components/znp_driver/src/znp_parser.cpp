// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_parser.cpp — MT wire-format encode/decode. SOF and FCS live here and
// only here; higher layers see only ZnpFrame.

#include "znp_internal.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "znp_parser";

static uint8_t fcs_calc(uint8_t len, uint8_t cmd0, uint8_t cmd1,
                        const uint8_t* data, uint8_t data_len) {
    uint8_t fcs = len ^ cmd0 ^ cmd1;
    for (uint8_t i = 0; i < data_len; i++) fcs ^= data[i];
    return fcs;
}

size_t znp_mt_encode(const ZnpFrame& f, uint8_t* buf, size_t buf_size) {
    const size_t total = MT_OVERHEAD + f.len;
    if (total > buf_size) return 0;
    buf[0] = MT_SOF;
    buf[1] = f.len;
    buf[2] = f.cmd0;
    buf[3] = f.cmd1;
    if (f.len > 0) memcpy(buf + 4, f.data, f.len);
    buf[4 + f.len] = fcs_calc(f.len, f.cmd0, f.cmd1, f.data, f.len);
    return total;
}

bool znp_is_expected_srsp(uint8_t req_cmd0, uint8_t req_cmd1,
                          uint8_t rsp_cmd0, uint8_t rsp_cmd1) {
    if ((rsp_cmd0 & 0xE0) != 0x60)                     return false; // not SRSP
    if ((rsp_cmd0 & 0x1F) != (req_cmd0 & 0x1F))        return false; // subsystem
    return rsp_cmd1 == req_cmd1;
}

void MtStreamParser::reset() {
    st_ = St::Sof;
    len_ = got_ = cmd0_ = cmd1_ = 0;
}

void MtStreamParser::feed(uint8_t b, Callback cb, void* ctx) {
    switch (st_) {
        case St::Sof:
            if (b == MT_SOF) st_ = St::Len;
            break;
        case St::Len:
            len_ = b;
            if (len_ > ZNP_MAX_DATA_LEN) { reset(); break; }  // impossible
            got_ = 0;
            st_ = St::Cmd0;
            break;
        case St::Cmd0:
            cmd0_ = b;
            st_ = St::Cmd1;
            break;
        case St::Cmd1:
            cmd1_ = b;
            st_ = (len_ == 0) ? St::Fcs : St::Data;
            break;
        case St::Data:
            data_[got_++] = b;
            if (got_ >= len_) st_ = St::Fcs;
            break;
        case St::Fcs: {
            const uint8_t expected = fcs_calc(len_, cmd0_, cmd1_, data_, len_);
            if (b == expected) {
                ZnpFrame out{};
                out.cmd0 = cmd0_;
                out.cmd1 = cmd1_;
                out.len  = len_;
                if (len_) memcpy(out.data, data_, len_);
                cb(out, ctx);
            } else {
                ESP_LOGW(TAG, "FCS mismatch cmd0=0x%02x cmd1=0x%02x",
                         cmd0_, cmd1_);
                znp_stats_bump(ZnpStat::BadFrame);
            }
            reset();
            break;
        }
    }
}
