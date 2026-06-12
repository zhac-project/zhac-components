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
    // §4 (FINDINGS.md): a plain fetch_add wraps 0xFF→0x00, so every 256th
    // frame is handed TSN 0. Throughout the stack 0 is the reserved
    // "not-set" / placeholder TSN (e.g. zhc_send_bridge writes 0 before we
    // patch the real seq in; the confirm ring treats trans_id as a live
    // key). A frame that legitimately carries 0 cannot be distinguished
    // from an un-patched placeholder and mis-correlates against a stale
    // confirm slot. CAS loop: take the next value, and if it landed on 0
    // bump once more so the returned TSN is always 1..255.
    uint8_t cur = s_zcl_seq.load(std::memory_order_relaxed);
    for (;;) {
        uint8_t next = static_cast<uint8_t>(cur + 1);
        if (next == 0) next = 1;   // skip the reserved 0
        if (s_zcl_seq.compare_exchange_weak(cur, next,
                                            std::memory_order_relaxed)) {
            return next;
        }
        // cur was reloaded by compare_exchange_weak with the current value;
        // retry. (weak may also fail spuriously — the loop handles both.)
    }
}
