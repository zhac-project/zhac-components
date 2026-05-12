// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

// Late-identity enrichment path (Step 2 of the backend-agnostic lifecycle).
//
// During the synchronous interview we try to read Basic cluster attributes
// 0x0004 (manufacturerName) / 0x0005 (modelIdentifier). Many devices —
// especially sleepy end-devices — miss that window. When they later send a
// ZCL Report Attributes or respond to a read on cluster 0x0000, we reuse
// the same identity fields to promote the device from IDENTITY_PENDING to
// IDENTITY_READY and trigger re-match without user intervention.
//
// The RX task calls zigbee_identity_on_af_incoming() to deliver a compact
// snapshot to a worker task; the worker updates the pool, persists the
// record once per identity change, and runs the matcher/configure path.

void zigbee_identity_init();

// Called from the UART RX task for every AF_INCOMING_MSG. Cheap no-op when
// cluster != 0x0000 or the frame is not a Basic identity report/response.
// Never blocks — posts to the identity queue with zero-timeout.
void zigbee_identity_on_af_incoming(const uint8_t* af_payload, uint8_t af_payload_len);
