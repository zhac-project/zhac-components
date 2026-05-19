// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hap_protocol.h"
#include "metrics/metrics_macros.h"

inline HapDecodeResult hap_decode_with_counters(
    const uint8_t* buf, size_t len, HapFrame& out, size_t* consumed) {
    HapDecodeResult r = hap_decode_stream(buf, len, out, consumed);
    if (r == HAP_DECODE_OK) return r;
    if (r == HAP_DECODE_TRUNCATED) {
        _METRIC_COUNTER_INC(METRIC_HAP_TRUNCATED, 1);
        return r;
    }
    if (r == HAP_DECODE_CRC_ERROR)
        _METRIC_COUNTER_INC(METRIC_HAP_CRC_ERRORS, 1);
    else if (r == HAP_DECODE_BAD_MAGIC)
        _METRIC_COUNTER_INC(METRIC_HAP_BAD_MAGIC, 1);
    else if (r == HAP_DECODE_BAD_VERSION)
        _METRIC_COUNTER_INC(METRIC_HAP_VERSION_MISMATCH, 1);
    else if (r == HAP_DECODE_BAD_HDR_CRC)
        _METRIC_COUNTER_INC(METRIC_HAP_HDR_CRC_ERRORS, 1);
    // HAP_DECODE_RESYNC: no per-failure counter — RESYNC_BYTES below
    // captures the data loss, and the upstream error metric was already
    // bumped when the original failed decode happened.
    if (consumed && *consumed > 0)
        _METRIC_COUNTER_INC(METRIC_HAP_RESYNC_BYTES, *consumed);
    return r;
}
