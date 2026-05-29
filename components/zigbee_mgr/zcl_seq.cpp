// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Monotonic ZCL transaction-sequence counter. Declared in zcl_seq.h
// and used by every outbound ZCL frame so responses can be correlated.
#include "zcl_seq.h"
#include <atomic>

// F20 (FINDINGS.md): atomic — outbound ZCL frames are emitted from more
// than one task (interview, send bridge, rules/cron), so a plain `++`
// read-modify-write could hand two frames the same sequence number and
// mis-correlate their responses.
static std::atomic<uint8_t> s_zcl_seq{0};

uint8_t zcl_seq_next() {
    // fetch_add returns the previous value; +1 preserves the original
    // pre-increment semantics (first call returns 1).
    return static_cast<uint8_t>(s_zcl_seq.fetch_add(1, std::memory_order_relaxed) + 1);
}
