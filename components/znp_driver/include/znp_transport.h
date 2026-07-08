// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_transport.h — public synchronous API for the ZNP transport.
//
// Callers use znp_call() for SREQ → SRSP and znp_subscribe_areq() for AREQ.
// All UART access is funneled through a single worker task (znp_worker.cpp)
// and a single RX task (znp_rx.cpp); no other task touches UART.

#pragma once
#include "znp_types.h"

// Bring the transport up: configure UART pins, install driver, release
// nRESET, spawn RX + worker tasks. Idempotent.
void znp_transport_start();

// Synchronous SREQ → SRSP. Blocks the calling task until one of:
//   - a matching SRSP arrives           → ZnpStatus::OK, srsp_out populated
//   - SYS_RESET_IND arrives mid-call    → ZnpStatus::RESET_DURING_CALL
//   - timeout_ms elapses                → ZnpStatus::TIMEOUT
//   - UART TX fails                     → ZnpStatus::UART_TX_ERROR
//   - transport isn't usable            → ZnpStatus::TRANSPORT_DOWN
//
// Late/unexpected SRSP is NEVER delivered as the reply — it's dropped by
// the worker and counted in stats.late_srsp / stats.unexpected_srsp.
ZnpStatus znp_call(uint8_t cmd0, uint8_t cmd1,
                   const uint8_t* data, uint8_t data_len,
                   ZnpFrame& srsp_out, uint32_t timeout_ms);

// Register an AREQ handler. cmd0 must carry the AREQ type bits (0x40|subsys).
// One handler per (cmd0,cmd1): re-subscribing the same pair REPLACES the
// previous handler (last registration wins), and znp_areq_dispatch delivers
// each frame to that single matching handler — it does NOT fan out. Every
// current subscription uses a distinct pair, so this is sufficient.
void znp_subscribe_areq(uint8_t cmd0, uint8_t cmd1, ZnpAreqHandler cb);

// Synthesise an AREQ dispatch. Normal RX path calls this; exposed here
// so unit tests can inject a frame without running the UART RX loop.
void znp_areq_dispatch(const ZnpFrame& f);

// Hardware-reset the NCP by asserting nRESET low for 10 ms.
void znp_hw_reset();

// Diagnostics — safe to call from any task.
ZnpTransportState znp_get_state();
ZnpTransportStats znp_get_stats();

// Retry-capable variant of znp_call. Returns the status of the last attempt.
// Logs a warning on each failure. Non-recoverable statuses (TRANSPORT_DOWN,
// INTERNAL_ERROR) fail fast without further retries.
ZnpStatus znp_call_retry(uint8_t cmd0, uint8_t cmd1,
                         const uint8_t* data, uint8_t data_len,
                         ZnpFrame& srsp_out,
                         uint32_t timeout_ms, int max_attempts);

// SRSP-matching helper, exposed for unit testing and for callers that need
// to reason about MT type/subsystem bits. Pure function, no side effects.
// Returns true when rsp_cmd0/rsp_cmd1 is a valid SRSP for req_cmd0/req_cmd1.
bool znp_is_expected_srsp(uint8_t req_cmd0, uint8_t req_cmd1,
                          uint8_t rsp_cmd0, uint8_t rsp_cmd1);
