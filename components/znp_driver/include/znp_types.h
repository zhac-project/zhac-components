// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_types.h — high-level types for the ZNP transport layer.
//
// These types are the vocabulary the rest of the firmware uses to talk to
// the TI Z-Stack NCP. They intentionally contain NO transport-level framing:
// SOF and FCS are encoder/parser details (see znp_parser.cpp) and never leak
// into ZnpFrame.
//
// The key invariant is ownership: ZnpFrame carries its own `data[]` buffer,
// not a pointer into a shared RX buffer. This is what fixes the "two callers
// stomp on the same s_srsp_buf" race that the old znp_driver.cpp had.

#pragma once

#include "znp_driver.h"   // legacy MT_* constants + MtFrame (kept for shim)

#include <cstdint>
#include <cstddef>
#include <functional>

// Maximum MT payload (data) length. SOF and FCS are transport-only.
static constexpr size_t ZNP_MAX_DATA_LEN = MT_MAX_PAYLOAD;

// Owning MT frame used throughout the transport layer.
struct ZnpFrame {
    uint8_t cmd0;
    uint8_t cmd1;
    uint8_t len;                         // data bytes only, 0..ZNP_MAX_DATA_LEN
    uint8_t data[ZNP_MAX_DATA_LEN];
};

// Result of a synchronous ZNP call.
enum class ZnpStatus : uint8_t {
    OK                   = 0,
    TIMEOUT              = 1,
    UART_TX_ERROR        = 2,
    RESET_DURING_CALL    = 3,   // NCP reset indication arrived mid-call
    TRANSPORT_DOWN       = 4,   // transport not up, or queue refused
    INTERNAL_ERROR       = 5,   // allocation failure, bad args, etc.
    UNEXPECTED_RESPONSE  = 6,   // reserved; worker logs+counts instead today
};

struct ZnpReply {
    ZnpStatus status;
    ZnpFrame  srsp;    // populated only when status == OK
};

// RX-side classification of a parsed MT frame.
enum class ZnpRxEventType : uint8_t {
    Srsp,
    Areq,
    ResetInd,       // SYS_RESET_IND — special transport event
    ParseError,     // FCS/SOF/truncation — frame discarded
};

struct ZnpRxEvent {
    ZnpRxEventType type;
    ZnpFrame       frame;    // valid for Srsp/Areq/ResetInd
};

// Transport lifecycle. Only znp_state.cpp mutates it; everything else reads.
enum class ZnpTransportState : uint8_t {
    Down       = 0,
    Booting    = 1,
    Init       = 2,
    Up         = 3,
    Recovering = 4,
    Error      = 5,
};

struct ZnpTransportStats {
    uint32_t tx_sreq_count;
    uint32_t rx_srsp_count;
    uint32_t rx_areq_count;
    uint32_t resets_seen;
    uint32_t timeouts;
    uint32_t unexpected_srsp;
    uint32_t late_srsp;
    uint32_t bad_frames;
    uint32_t tx_errors;
    uint32_t recoveries;
    uint32_t duplicate_areqs;  // ZDO response AREQs dropped by the dedup ring
};

// AREQ subscriber.
using ZnpAreqHandler = std::function<void(const ZnpFrame&)>;
