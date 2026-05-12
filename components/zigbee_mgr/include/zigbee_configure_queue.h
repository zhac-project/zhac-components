// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

// Post-interview configure queue (Step 3 of the backend-agnostic lifecycle).
//
// Separates the potentially slow, retry-friendly configure step (binding,
// attribute reporting setup, cluster-specific writes) from the synchronous
// interview. A dedicated worker owns configure so that:
//
//   - the interview task never blocks on a flaky bind/configure round-trip;
//   - failed configures are retried with exponential backoff without
//     spinning the RX path;
//   - successful configures are deduped — ConfigureState::DONE is terminal
//     and the worker early-exits without re-binding a device on every
//     rejoin (prevents wasting binding-table slots on constrained devices).

void zigbee_configure_init();

// Enqueue a device for (re-)configure. Idempotent: if the device is already
// ConfigureState::DONE the worker will skip it, so callers don't need to
// dedupe themselves.
void zigbee_configure_enqueue(uint64_t ieee);
