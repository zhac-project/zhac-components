// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Metrics engine public API. All functions are `noexcept`. When
// `CONFIG_METRICS_ENABLED` is n, stub implementations are linked so
// callers don't need to conditionally compile call sites that go
// through the public API directly (macro users get compile-time
// no-ops — see metrics_macros.h).

#include "metrics/metrics_types.h"
#include <cstdint>

namespace metrics {

// Zero-initialise shard storage. Safe to call once at boot; idempotent.
void init() noexcept;

// Counter updates (monotonic). `counter_set` replaces the aggregate
// value — all other shards are cleared so subsequent reads return the
// externally-set value exactly.
void counter_inc(MetricId id, uint64_t delta = 1) noexcept;
void counter_set(MetricId id, uint64_t value) noexcept;

// Record a sampled signed value (RSSI, queue depth, payload size, …).
void value_record(MetricId id, int64_t sample) noexcept;

// Timer API. The RAII `TimerScope` is the preferred entry point; the
// free functions exist for code paths that can't fit a scope cleanly.
TimerToken timer_start(MetricId id) noexcept;
void       timer_stop(TimerToken& token) noexcept;
void       timer_record(MetricId id, uint32_t duration_us) noexcept;

class TimerScope {
public:
    explicit TimerScope(MetricId id) noexcept;
    ~TimerScope() noexcept;
    TimerScope(const TimerScope&)            = delete;
    TimerScope& operator=(const TimerScope&) = delete;

private:
    TimerToken token_;
};

// Aggregated snapshot readers — walk all shards and return a merged
// view. Returns false if `id` does not map to the requested kind.
bool read_timer(MetricId id, TimerSnapshot& out) noexcept;
bool read_counter(MetricId id, CounterSnapshot& out) noexcept;
bool read_value(MetricId id, ValueSnapshot& out) noexcept;

// Populate the heap value metrics from ESP-IDF heap APIs. No-op when
// CONFIG_METRICS_ENABLE_MEMORY_METRICS=n.
void update_memory_snapshot() noexcept;

// Format a human-readable dump into `buf`. Output is always
// NUL-terminated when `buf_size > 0`. Returns number of bytes
// populated, excluding the NUL. Returns 0 when
// CONFIG_METRICS_ENABLE_TEXT_DUMP=n or on zero-size buffer.
size_t dump_text(char* buf, size_t buf_size) noexcept;

}  // namespace metrics
