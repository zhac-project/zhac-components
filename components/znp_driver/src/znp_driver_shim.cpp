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

// Retained for any future shim-local logging; the retry path now delegates
// to znp_call_retry which logs under its own tag, so this is currently
// unreferenced (the only call sites that used it moved out).
[[maybe_unused]] static const char* TAG = "znp_shim";

// ── Legacy mt_fcs / mt_encode / mt_decode ─────────────────────────────────
// Used by tests and a handful of callers that parse ad-hoc buffers. §4
// (FINDINGS.md): the FCS + encode framing used to be a SECOND, independent
// copy of the wire format that already lives in znp_parser.cpp — two paths
// that could silently drift. They now delegate to the parser's single
// owner (znp_mt_fcs / znp_mt_encode) so there is exactly one definition of
// the on-wire MT frame. mt_decode stays here: the parser only offers a
// streaming decoder, but it shares the same FCS via znp_mt_fcs below.
uint8_t mt_fcs(uint8_t len, uint8_t cmd0, uint8_t cmd1,
               const uint8_t* payload, uint8_t payload_len) {
    return znp_mt_fcs(len, cmd0, cmd1, payload, payload_len);
}

size_t mt_encode(const MtFrame& f, uint8_t* buf, size_t buf_size) {
    // Bridge the legacy MtFrame into the owning ZnpFrame encoder. ZnpFrame
    // carries its own data[]; copy the (non-owning) MtFrame payload in.
    if (f.payload_len > ZNP_MAX_DATA_LEN) return 0;
    ZnpFrame zf{};
    zf.cmd0 = f.cmd0;
    zf.cmd1 = f.cmd1;
    zf.len  = f.payload_len;
    if (f.payload_len > 0 && f.payload) memcpy(zf.data, f.payload, f.payload_len);
    return znp_mt_encode(zf, buf, buf_size);
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
    // §4 (FINDINGS.md, znp_driver_shim.cpp:85): this used to loop on
    // znp_sreq with NO inter-attempt backoff and retried on EVERY failure
    // — including RESET_DURING_CALL — so a legacy caller hammered a
    // resetting NCP three times back-to-back, bypassing the F43 backoff
    // that znp_call_retry already implements. Route through znp_call_retry
    // instead: it backs off 25/50/100/200 ms between attempts and fails
    // fast on TRANSPORT_DOWN / INTERNAL_ERROR. (RESET_DURING_CALL is
    // retried by design there — the NCP is coming back up — but now with
    // backoff rather than an immediate re-send into the reset window.)
    const ZnpStatus st = znp_call_retry(req.cmd0, req.cmd1,
                                        req.payload, req.payload_len,
                                        tls_srsp_buf, timeout_ms, max_attempts);
    if (st != ZnpStatus::OK) return false;
    srsp_out.cmd0        = tls_srsp_buf.cmd0;
    srsp_out.cmd1        = tls_srsp_buf.cmd1;
    srsp_out.payload     = tls_srsp_buf.data;
    srsp_out.payload_len = tls_srsp_buf.len;
    return true;
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
