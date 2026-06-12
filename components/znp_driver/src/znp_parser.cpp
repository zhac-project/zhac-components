// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_parser.cpp — MT wire-format encode/decode. SOF and FCS live here and
// only here; higher layers see only ZnpFrame.

#include "znp_internal.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "znp_parser";

// MT XOR-8 FCS. Non-static + declared in znp_internal.h so the legacy
// shim (znp_driver_shim.cpp) delegates here instead of carrying a second
// copy that could drift from the wire format the parser enforces (§4).
uint8_t znp_mt_fcs(uint8_t len, uint8_t cmd0, uint8_t cmd1,
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
    buf[4 + f.len] = znp_mt_fcs(f.len, f.cmd0, f.cmd1, f.data, f.len);
    return total;
}

// §4 (FINDINGS.md, zigbee_mgr.cpp:405 SEC): the Zigbee network key
// (PRECFGKEY) and the active/alternate network-key-info NV items are
// hex-dumped in PLAINTEXT by the wire trace when it's on (znp_worker TX +
// znp_rx RX). A capture taken for debugging would then contain the live
// network key. This predicate flags SYS_OSAL_NV_WRITE / SYS_OSAL_NV_WRITE_EXT
// frames whose target item id is a key item, so the trace can redact the
// data while still printing the header. Item ids (z-stack-3.x):
//   0x0062 PRECFGKEY, 0x003A NWK_ACTIVE_KEY_INFO, 0x003B NWK_ALTERN_KEY_INFO.
// MT: SYS subsystem = 0x01; NV_WRITE cmd1 = 0x09, NV_WRITE_EXT cmd1 = 0x1D.
// Both carry the item id in the first two payload bytes (LE).
bool znp_wire_is_sensitive(uint8_t cmd0, uint8_t cmd1,
                           const uint8_t* data, uint8_t len) {
    if ((cmd0 & 0x1F) != 0x01)            return false;  // not SYS subsystem
    if (cmd1 != 0x1D && cmd1 != 0x09)     return false;  // not an NV write
    if (len < 2)                          return false;
    const uint16_t id = static_cast<uint16_t>(data[0]) |
                        (static_cast<uint16_t>(data[1]) << 8);
    return id == 0x0062 || id == 0x003A || id == 0x003B;
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
            const uint8_t expected = znp_mt_fcs(len_, cmd0_, cmd1_, data_, len_);
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
