// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/hap_session/include/hap_session.h
#pragma once
#include "hap_protocol.h"
#include <functional>

// Frame flags (stored in HapFrame.flags)
inline constexpr uint8_t HAP_FLAG_NEEDS_ACK = 0x01;  // sender expects ACK echo
inline constexpr uint8_t HAP_FLAG_NO_ACK    = 0x02;  // fire-and-forget (large frames)

using HapSendFn       = std::function<void(const HapFrame&)>;
using HapFrameHandler = std::function<void(const HapFrame&)>;

struct HapSessionCfg {
    HapSendFn             send;           // underlying transport (hap_slave_send or hap_master_send)
    HapFrameHandler       on_frame;       // called for validated application frames
    HapFrameHandler       on_sync;        // called when a SYNC frame arrives
    std::function<void()> on_link_dead;   // called after 3 retransmit failures
};

// Initialise session with given config. Resets all window state.
void hap_session_init(const HapSessionCfg& cfg);

// Queue frame for send. Returns false if window full (only for NEEDS_ACK frames).
bool hap_session_send(const HapFrame& frame);

// Feed a decoded frame from the transport. Routes ACKs, SYNCs, and data frames.
void hap_session_on_receive(const HapFrame& frame);

// Call every ~10 ms from TaskHAP. Checks ~1000 ms ACK timeouts, retransmits
// up to 5× (total budget ~5 s). See hap_session.cpp ACK_TIMEOUT_MS / MAX_RETRIES.
void hap_session_tick();

// Returns and auto-increments the next outgoing sequence number.
uint16_t hap_session_next_seq();
