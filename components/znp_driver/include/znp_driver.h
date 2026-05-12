// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

static constexpr uint8_t  MT_SOF         = 0xFE;
static constexpr size_t   MT_OVERHEAD    = 5;
static constexpr size_t   MT_MAX_PAYLOAD = 250;
static constexpr size_t   MT_MAX_FRAME   = MT_OVERHEAD + MT_MAX_PAYLOAD;

static constexpr uint8_t MT_SREQ(uint8_t sub) { return static_cast<uint8_t>(0x20 | sub); }
static constexpr uint8_t MT_AREQ(uint8_t sub) { return static_cast<uint8_t>(0x40 | sub); }
static constexpr uint8_t MT_SRSP(uint8_t sub) { return static_cast<uint8_t>(0x60 | sub); }

static constexpr uint8_t ZNP_SYS     = 0x01;
static constexpr uint8_t ZNP_AF      = 0x04;
static constexpr uint8_t ZNP_ZDO     = 0x05;
static constexpr uint8_t ZNP_UTIL    = 0x07;
static constexpr uint8_t ZNP_APP_CNF = 0x0F;

struct MtFrame {
    uint8_t        cmd0;
    uint8_t        cmd1;
    const uint8_t* payload;      // non-owning, points into rx buffer
    uint8_t        payload_len;
};

enum MtDecodeResult {
    MT_DECODE_OK        = 0,
    MT_DECODE_BAD_SOF   = 1,
    MT_DECODE_FCS_ERROR = 2,
    MT_DECODE_TRUNCATED = 3,
    MT_DECODE_OVERFLOW  = 4,
};

using MtAreqCallback = std::function<void(const MtFrame&)>;

uint8_t mt_fcs(uint8_t len, uint8_t cmd0, uint8_t cmd1,
               const uint8_t* payload, uint8_t payload_len);

size_t mt_encode(const MtFrame& frame, uint8_t* buf, size_t buf_size);
MtDecodeResult mt_decode(const uint8_t* buf, size_t len, MtFrame& out);

void znp_driver_init();
bool znp_sreq(const MtFrame& req, MtFrame& srsp_out, uint32_t timeout_ms = 2000);

// Retry wrapper: attempts znp_sreq up to max_attempts times.
// Returns true on first success; logs a warning on each failure.
bool znp_sreq_retry(const MtFrame& req, MtFrame& srsp_out,
                    uint32_t timeout_ms = 2000, int max_attempts = 3);

void znp_register_areq(uint8_t cmd0, uint8_t cmd1, MtAreqCallback cb);

// Hardware reset CC2652 — drives nRESET low for 10 ms then releases.
void znp_hw_reset();

// Wire-level packet trace. When enabled, every outbound and inbound MT
// frame is hex-dumped at INFO level under tag "znp_wire". Off by
// default. Toggle at runtime (REST / shell / debug hook) — same tag
// pairs TX and RX so one view captures both directions.
void znp_set_wire_trace(bool enabled);
bool znp_get_wire_trace();
