// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mock ZNP transport for the zigbee_mgr host harness. Implements the functions
// declared in znp_driver.h + znp_confirm.h WITHOUT the wire codec / UART —
// znp_driver.cpp is deliberately NOT compiled. See znp_stub.h for the rationale
// and the test-facing control surface.
#include "znp_stub.h"
#include "znp_confirm.h"

#include <cstring>
#include <vector>

namespace {

// ── Recording of outbound SREQ requests ─────────────────────────────────
std::vector<ZnpRecordedReq> g_reqs;

// ── Responder + SRSP scratch ────────────────────────────────────────────
ZnpResponder g_responder;
uint8_t      g_srsp_buf[256];
const uint8_t kOkStatus[1] = {0x00};
int          g_confirm_status = 0;

// ── Registered AREQ callbacks ───────────────────────────────────────────
struct AreqEntry { uint8_t cmd0; uint8_t cmd1; MtAreqCallback cb; };
std::vector<AreqEntry> g_areqs;

// Core SREQ handling shared by znp_sreq + znp_sreq_retry: record the request,
// then produce an SRSP via the responder (or the default success reply).
bool do_sreq(const MtFrame& req, MtFrame& srsp_out) {
    ZnpRecordedReq r{};
    r.cmd0 = req.cmd0;
    r.cmd1 = req.cmd1;
    r.payload_len = req.payload_len;
    // payload_len is uint8_t (<=255) and r.payload is 256 bytes, so the copy
    // always fits — no clamp needed.
    if (req.payload && req.payload_len) {
        std::memcpy(r.payload, req.payload, req.payload_len);
    }
    g_reqs.push_back(r);

    srsp_out = MtFrame{};
    // SRSP mirrors the request's subsystem/command per MT convention.
    srsp_out.cmd0 = MT_SRSP(static_cast<uint8_t>(req.cmd0 & 0x1F));
    srsp_out.cmd1 = req.cmd1;

    if (g_responder) return g_responder(r, srsp_out);

    // Default: single status=0x00 success byte.
    znp_stub_set_srsp(srsp_out, kOkStatus, 1);
    return true;
}

}  // namespace

// ── znp_stub control surface ────────────────────────────────────────────
void znp_stub_reset() {
    g_reqs.clear();
    g_responder = nullptr;
    g_confirm_status = 0;
    // g_areqs intentionally preserved (registrations live for the process).
}

int znp_stub_req_count() { return static_cast<int>(g_reqs.size()); }

const ZnpRecordedReq& znp_stub_req(int idx) {
    static const ZnpRecordedReq kEmpty{};
    if (idx < 0 || idx >= static_cast<int>(g_reqs.size())) return kEmpty;
    return g_reqs[static_cast<size_t>(idx)];
}

const ZnpRecordedReq& znp_stub_last() {
    static const ZnpRecordedReq kEmpty{};
    if (g_reqs.empty()) return kEmpty;
    return g_reqs.back();
}

void znp_stub_set_responder(ZnpResponder r) { g_responder = std::move(r); }

void znp_stub_set_srsp(MtFrame& srsp, const uint8_t* data, uint8_t len) {
    // len is uint8_t (<=255); g_srsp_buf is 256 bytes, so it always fits.
    if (data && len) std::memcpy(g_srsp_buf, data, len);
    srsp.payload = g_srsp_buf;
    srsp.payload_len = len;
}

void znp_stub_set_confirm_status(int status) { g_confirm_status = status; }

bool znp_stub_inject_areq(uint8_t cmd0, uint8_t cmd1,
                          const uint8_t* payload, uint8_t len) {
    static uint8_t buf[256];
    if (len && payload) std::memcpy(buf, payload, len);   // len<=255 fits buf[256]
    MtFrame f{};
    f.cmd0 = cmd0;
    f.cmd1 = cmd1;
    f.payload = len ? buf : nullptr;
    f.payload_len = len;
    bool any = false;
    for (const auto& e : g_areqs) {
        if (e.cmd0 == cmd0 && e.cmd1 == cmd1 && e.cb) { e.cb(f); any = true; }
    }
    return any;
}

// ── znp_driver.h implementation (mocked) ────────────────────────────────
void znp_driver_init() {}

bool znp_sreq(const MtFrame& req, MtFrame& srsp_out, uint32_t /*timeout_ms*/) {
    return do_sreq(req, srsp_out);
}

bool znp_sreq_retry(const MtFrame& req, MtFrame& srsp_out,
                    uint32_t /*timeout_ms*/, int /*max_attempts*/) {
    return do_sreq(req, srsp_out);
}

void znp_register_areq(uint8_t cmd0, uint8_t cmd1, MtAreqCallback cb) {
    g_areqs.push_back({cmd0, cmd1, std::move(cb)});
}

// Faithful mock: a hardware reset of the CC2652 always causes it to emit a
// SYS_RESET_IND. coordinator_start() polls s_reset_received (set only by that
// AREQ) with vTaskDelay — a no-op on the host — so without this the startup
// reset wait would spin-timeout. Firing the indication here lets init proceed.
void znp_hw_reset() {
    znp_stub_inject_areq(MT_AREQ(ZNP_SYS), 0x80, nullptr, 0);
}

// ── znp_confirm.h implementation (mocked) ───────────────────────────────
// Single-slot model: reserve always succeeds (slot 0), wait returns the
// test-settable MAC status (default 0 = delivered), release is a no-op.
extern "C" int znp_confirm_reserve(uint8_t /*trans_id*/) { return 0; }
extern "C" int znp_confirm_wait(int /*slot*/, uint32_t /*timeout_ms*/) {
    return g_confirm_status;
}
extern "C" void znp_confirm_release(int /*slot*/) {}
