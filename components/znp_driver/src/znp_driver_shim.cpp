// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_driver_shim.cpp — back-compat for the old MtFrame / znp_sreq API.
//
// The transport internals have been rewritten (see znp_transport.cpp) but
// ~10 files in zigbee_mgr and zcl_converter still call the original API.
// This shim bridges them without forcing a mass edit.
//
// Lifetime note for response payloads:
//   The legacy MtFrame carries a NON-owning `payload` pointer. We satisfy
//   the old contract by pointing it into a thread_local ZnpFrame. That is
//   safe because znp_sreq is synchronous — the buffer is valid from return
//   of znp_sreq until the same task calls znp_sreq again. Unlike the old
//   global s_srsp_buf, each task now has its own slot, so two concurrent
//   callers cannot overwrite each other's response.

#include "znp_driver.h"
#include "znp_transport.h"
#include "znp_internal.h"
#include "esp_log.h"
#include <cstring>
#include <utility>

static const char* TAG = "znp_shim";

// ── Legacy mt_fcs / mt_encode / mt_decode ─────────────────────────────────
// Used by tests and a handful of callers that parse ad-hoc buffers. Kept
// behaviorally identical; the new parser uses its own private FCS helper so
// the two code paths never disagree about the wire format.
uint8_t mt_fcs(uint8_t len, uint8_t cmd0, uint8_t cmd1,
               const uint8_t* payload, uint8_t payload_len) {
    uint8_t fcs = len ^ cmd0 ^ cmd1;
    for (uint8_t i = 0; i < payload_len; i++) fcs ^= payload[i];
    return fcs;
}

size_t mt_encode(const MtFrame& f, uint8_t* buf, size_t buf_size) {
    const size_t total = MT_OVERHEAD + f.payload_len;
    if (total > buf_size) return 0;
    buf[0] = MT_SOF;
    buf[1] = f.payload_len;
    buf[2] = f.cmd0;
    buf[3] = f.cmd1;
    if (f.payload_len > 0 && f.payload) memcpy(buf + 4, f.payload, f.payload_len);
    buf[4 + f.payload_len] = mt_fcs(f.payload_len, f.cmd0, f.cmd1,
                                     f.payload, f.payload_len);
    return total;
}

MtDecodeResult mt_decode(const uint8_t* buf, size_t len, MtFrame& out) {
    if (len < MT_OVERHEAD)     return MT_DECODE_TRUNCATED;
    if (buf[0] != MT_SOF)      return MT_DECODE_BAD_SOF;
    uint8_t plen = buf[1];
    if (plen > MT_MAX_PAYLOAD) return MT_DECODE_OVERFLOW;
    if (len < (size_t)(MT_OVERHEAD + plen)) return MT_DECODE_TRUNCATED;
    uint8_t expected = mt_fcs(plen, buf[2], buf[3], buf + 4, plen);
    if (expected != buf[4 + plen]) return MT_DECODE_FCS_ERROR;
    out.cmd0        = buf[2];
    out.cmd1        = buf[3];
    out.payload     = buf + 4;
    out.payload_len = plen;
    return MT_DECODE_OK;
}

// ── init ─────────────────────────────────────────────────────────────────
void znp_driver_init() { znp_transport_start(); }

// ── znp_sreq / znp_sreq_retry ─────────────────────────────────────────────
static thread_local ZnpFrame tls_srsp_buf;

bool znp_sreq(const MtFrame& req, MtFrame& srsp_out, uint32_t timeout_ms) {
    const ZnpStatus st = znp_call(req.cmd0, req.cmd1,
                                   req.payload, req.payload_len,
                                   tls_srsp_buf, timeout_ms);
    if (st != ZnpStatus::OK) return false;
    srsp_out.cmd0        = tls_srsp_buf.cmd0;
    srsp_out.cmd1        = tls_srsp_buf.cmd1;
    srsp_out.payload     = tls_srsp_buf.data;
    srsp_out.payload_len = tls_srsp_buf.len;
    return true;
}

bool znp_sreq_retry(const MtFrame& req, MtFrame& srsp_out,
                    uint32_t timeout_ms, int max_attempts) {
    for (int i = 1; i <= max_attempts; i++) {
        if (znp_sreq(req, srsp_out, timeout_ms)) return true;
        ESP_LOGW(TAG, "SREQ attempt %d/%d failed cmd0=0x%02x cmd1=0x%02x",
                 i, max_attempts, req.cmd0, req.cmd1);
    }
    return false;
}

// ── AREQ subscription adapter ─────────────────────────────────────────────
void znp_register_areq(uint8_t cmd0, uint8_t cmd1, MtAreqCallback cb) {
    znp_subscribe_areq(cmd0, cmd1,
        [cb = std::move(cb)](const ZnpFrame& f) {
            MtFrame mf;
            mf.cmd0        = f.cmd0;
            mf.cmd1        = f.cmd1;
            mf.payload     = f.data;
            mf.payload_len = f.len;
            cb(mf);
        });
}
