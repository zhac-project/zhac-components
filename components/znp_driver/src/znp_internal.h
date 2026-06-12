// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_internal.h — private header shared by the transport .cpp files.
// Do not include from outside components/znp_driver/src.

#pragma once
#include "znp_types.h"
#include "znp_transport.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ── UART configuration (owned by znp_transport.cpp) ───────────────────────
extern uart_port_t znp_uart_port;
extern QueueHandle_t znp_uart_evt_q;   // Q32: UART event queue (overrun detection)

// ── Parser / encoder (znp_parser.cpp) ─────────────────────────────────────
// Encode a high-level ZnpFrame into MT wire bytes (SOF+LEN+CMD0+CMD1+data+FCS).
// Returns the number of bytes written, or 0 if the buffer is too small.
size_t znp_mt_encode(const ZnpFrame& f, uint8_t* buf, size_t buf_size);

// MT XOR-8 FCS over (len ^ cmd0 ^ cmd1 ^ data[]). Single owner of the wire
// FCS — the legacy mt_* shim delegates here so the two paths cannot drift.
uint8_t znp_mt_fcs(uint8_t len, uint8_t cmd0, uint8_t cmd1,
                   const uint8_t* data, uint8_t data_len);

// True when this MT frame carries the Zigbee network key in its payload
// (SYS NV write of a key item id). The wire trace uses this to redact the
// key data instead of hex-dumping it in plaintext (§4, zigbee_mgr.cpp:405).
bool znp_wire_is_sensitive(uint8_t cmd0, uint8_t cmd1,
                           const uint8_t* data, uint8_t len);

// Streaming byte-by-byte parser. Owned by the RX task; not thread-safe.
class MtStreamParser {
public:
    using Callback = void (*)(const ZnpFrame& f, void* ctx);
    void reset();
    void feed(uint8_t byte, Callback cb, void* ctx);
private:
    enum class St : uint8_t { Sof, Len, Cmd0, Cmd1, Data, Fcs };
    St      st_   = St::Sof;
    uint8_t len_  = 0;
    uint8_t got_  = 0;
    uint8_t cmd0_ = 0;
    uint8_t cmd1_ = 0;
    uint8_t data_[ZNP_MAX_DATA_LEN] = {};
};

// znp_is_expected_srsp() is declared in the public header znp_transport.h.

// ── RX task (znp_rx.cpp) ──────────────────────────────────────────────────
void znp_rx_task_start();

// ── Worker (znp_worker.cpp) ───────────────────────────────────────────────
void znp_worker_task_start();
// Called by RX task when it has classified a frame.
void znp_worker_deliver_srsp(const ZnpFrame& f);
void znp_worker_deliver_reset_ind(const ZnpFrame& f);

// ── AREQ dispatch (znp_areq_dispatch.cpp) ─────────────────────────────────
// Declaration lives in the public znp_transport.h so unit tests can inject
// frames; znp_rx.cpp already pulls that header in.

// ── State + stats (znp_state.cpp) ─────────────────────────────────────────
void znp_state_set(ZnpTransportState s);

// Called by the RX task on every SYS_RESET_IND. Drives the recovery state
// machine: first boot → Up; unexpected reset while Up → Recovering and
// records a timestamp; ≥3 resets inside a 10-second window → Error.
void znp_state_note_reset();

// Called by the worker whenever an SRSP arrives successfully. Clears a
// pending Recovering state so subsequent calls observe Up again.
void znp_state_note_ok();

enum class ZnpStat : uint8_t {
    TxSreq, RxSrsp, RxAreq, Reset, Timeout,
    UnexpectedSrsp, LateSrsp, BadFrame, TxError, Recovery,
    DuplicateAreq,
};
void znp_stats_bump(ZnpStat s);
