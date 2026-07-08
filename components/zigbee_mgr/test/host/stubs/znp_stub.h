// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Test-facing control surface for the mocked ZNP transport (znp_stub.cpp).
//
// The whole point of the zigbee_mgr host harness: zigbee_mgr talks to the TI
// radio through the STRUCTURED MtFrame layer (znp_driver.h + znp_confirm.h),
// not the wire codec. We implement those functions here instead of compiling
// znp_driver.cpp, so the coordinator manager / ZCL builders / interview state
// machine run on the host against a scriptable fake radio.
//
// Capabilities:
//   • RECORD every znp_sreq* request (cmd0/cmd1/payload) for assertion — the
//     mock captures exactly the bytes that would go on the UART to the CC2652.
//   • RESPOND via a test-settable responder (canned SRSPs) — e.g. so the
//     zigbee_mgr_init() startup SREQ ladder succeeds.
//   • INJECT AREQ indications (device-announce / join / leave / reset /
//     state-change) into the registered callbacks to drive the state machine.
#pragma once
#include "znp_driver.h"   // real header: MtFrame, MT_SREQ/AREQ/SRSP, ZNP_* subsystems
#include <cstdint>
#include <functional>

// ── Recorded outbound SREQ ──────────────────────────────────────────────
struct ZnpRecordedReq {
    uint8_t cmd0;
    uint8_t cmd1;
    uint8_t payload[256];
    uint8_t payload_len;
};

// Clear recorded requests + responder + confirm status between test groups.
// Does NOT clear AREQ registrations (those persist for the process, exactly
// like the firmware registers them once at init).
void znp_stub_reset();

// How many znp_sreq*/requests have been recorded since the last reset.
int  znp_stub_req_count();
// Recorded request by index (0-based) / the most recent one. Out-of-range
// returns a zeroed static — callers should gate on znp_stub_req_count().
const ZnpRecordedReq& znp_stub_req(int idx);
const ZnpRecordedReq& znp_stub_last();

// ── Responder ───────────────────────────────────────────────────────────
// The test programs canned SRSPs. Return true = "SRSP produced" (radio
// accepted + replied); return false = "no SRSP" (models a UART/NCP timeout,
// so znp_sreq_retry fails). Fill `srsp` via znp_stub_set_srsp().
// When no responder is installed the default reply is a 1-byte status=0x00
// (success) SRSP, which is what most zigbee_mgr status checks expect.
using ZnpResponder = std::function<bool(const ZnpRecordedReq& req, MtFrame& srsp)>;
void znp_stub_set_responder(ZnpResponder r);

// Point `srsp.payload` at a stub-owned static buffer holding a copy of `data`.
// MtFrame.payload is non-owning; only one SRSP is ever in flight in the
// single-threaded harness, so a static buffer is safe.
void znp_stub_set_srsp(MtFrame& srsp, const uint8_t* data, uint8_t len);

// Force the AF_DATA_CONFIRM (MAC delivery) status returned by
// znp_confirm_wait(). Default 0 = delivered OK. Set <0 to model a MAC
// timeout, or e.g. 0xF0 for ZMacTransactionExpired.
void znp_stub_set_confirm_status(int status);

// ── AREQ injection ──────────────────────────────────────────────────────
// Find the callback registered for (cmd0,cmd1) and invoke it with a
// synthesized MtFrame carrying `payload`. This is how the test drives
// device-announce (TC_DEV_IND), leave (ZDO_LEAVE_IND), reset (SYS_RESET_IND)
// and state-change indications into the state machine. Returns true if at
// least one matching callback ran.
bool znp_stub_inject_areq(uint8_t cmd0, uint8_t cmd1,
                          const uint8_t* payload, uint8_t len);

// ── zhc_adapter cache-invalidation spy (implemented in zhc_adapter_stub.cpp) ─
// zigbee_pool_remove() calls zhac_adapter_invalidate_def_cache(); the spy lets
// the test assert that hard-remove drops the adapter's cached definition.
uint64_t zhac_stub_last_invalidated_ieee();
int      zhac_stub_invalidate_count();
void     zhac_stub_reset();
