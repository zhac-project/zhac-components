// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>

// P3 observability: track frames the ZCL pipeline can't handle.
//
// Triggered when an AF_INCOMING_MSG passes all dispatch stages
// (FromRule → CommandRule → VendorRule → runtime translator) with no
// attribute emission. Often harmless (ZCL default responses, config
// reads the UI requested) but sometimes a genuine gap — a vendor
// converter we haven't extracted yet.
//
// Bounded PSRAM ring (32 slots, LRU-eviction on overflow). Exposed
// via /api/diagnostics/unhandled so an operator can decide whether to
// teach the pipeline a new pattern.

struct ZbUnhandledFrame {
    uint16_t cluster_id;
    uint16_t attr_or_cmd_id;   // attr_id for global commands, cmd_id for cluster-specific
    uint8_t  cluster_specific; // 1 if frame was cluster-specific
    uint8_t  _pad;
    uint32_t count;            // occurrences since last reset
    uint32_t last_seen_s;      // Unix epoch of the most recent hit
    uint64_t last_ieee;        // IEEE of the most recent reporting device
};

void zb_diag_init();

// Record an unhandled frame. cluster/attr_or_cmd identify the key;
// cluster_specific=1 for cluster-specific (frame_ctrl bit0 set).
void zb_diag_record_unhandled(uint16_t cluster_id,
                               uint16_t attr_or_cmd_id,
                               bool cluster_specific,
                               uint64_t ieee);

// Snapshot the current ring (caller-provided buffer). Returns count copied.
// Entries are ordered by most recent `last_seen_s` first.
uint16_t zb_diag_snapshot(ZbUnhandledFrame* out, uint16_t max);

void zb_diag_reset();
