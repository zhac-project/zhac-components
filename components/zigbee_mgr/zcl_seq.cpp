// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Monotonic ZCL transaction-sequence counter. Declared in zcl_seq.h
// and used by every outbound ZCL frame so responses can be correlated.
#include "zcl_seq.h"

static uint8_t s_zcl_seq = 0;

uint8_t zcl_seq_next() {
    return ++s_zcl_seq;
}
