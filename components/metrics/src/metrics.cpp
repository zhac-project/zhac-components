// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// metrics.cpp — core runtime. Holds the sharded storage, shard selection,
// and the public API (counters, values, timers, snapshot readers).
//
// Zero dynamic allocation. All storage is static.

#include "metrics/metrics.h"
#include "metrics_atomic.h"
#include "metrics_internal.h"

#include "sdkconfig.h"

#if CONFIG_METRICS_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#if CONFIG_METRICS_ENABLE_MEMORY_METRICS
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace metrics {
namespace {

// ── Descriptor table (X-macro expansion) ─────────────────────────────
constexpr MetricDescriptor kDescriptors[] = {
#define METRIC_TIMER(id, name, help)   { MetricId::id, MetricKind::Timer,   name, help },
#define METRIC_COUNTER(id, name, help) { MetricId::id, MetricKind::Counter, name, help },
#define METRIC_VALUE(id, name, help)   { MetricId::id, MetricKind::Value,   name, help },
#include "metrics/metric_registry.def"
#undef METRIC_TIMER
#undef METRIC_COUNTER
#undef METRIC_VALUE
};

constexpr size_t kMetricCount =
    sizeof(kDescriptors) / sizeof(kDescriptors[0]);

static_assert(kMetricCount == static_cast<size_t>(MetricId::_COUNT),
              "kDescriptors and MetricId::_COUNT drifted");

// ── Kind-local index mapping (constexpr at build time) ───────────────
struct Mapping {
    MetricKind kind;
    uint16_t   index;   // index within the kind-local storage array
};

constexpr size_t count_kind(MetricKind k) {
    size_t n = 0;
    for (const auto& d : kDescriptors) {
        if (d.kind == k) ++n;
    }
    return n;
}
constexpr size_t kTimerCount   = count_kind(MetricKind::Timer);
constexpr size_t kCounterCount = count_kind(MetricKind::Counter);
constexpr size_t kValueCount   = count_kind(MetricKind::Value);

constexpr std::array<Mapping, kMetricCount> build_mapping() {
    std::array<Mapping, kMetricCount> out{};
    uint16_t ti = 0, ci = 0, vi = 0;
    for (size_t i = 0; i < kMetricCount; ++i) {
        switch (kDescriptors[i].kind) {
            case MetricKind::Timer:
                out[i] = { MetricKind::Timer,   ti++ };
                break;
            case MetricKind::Counter:
                out[i] = { MetricKind::Counter, ci++ };
                break;
            case MetricKind::Value:
                out[i] = { MetricKind::Value,   vi++ };
                break;
        }
    }
    return out;
}
constexpr auto kMapping = build_mapping();

// ── Shards & storage ─────────────────────────────────────────────────
constexpr size_t kShards = CONFIG_METRICS_NUM_SHARDS;
static_assert(kShards >= 1 && kShards <= 4, "bad CONFIG_METRICS_NUM_SHARDS");

struct TimerShard {
    std::atomic<uint64_t> count;
    std::atomic<uint64_t> sum_us;
    std::atomic<uint32_t> min_us;
    std::atomic<uint32_t> max_us;
    std::atomic<uint32_t> last_us;
};

struct CounterShard {
    std::atomic<uint64_t> value;
};

struct ValueShard {
    std::atomic<uint64_t> count;
    std::atomic<int64_t>  sum;
    std::atomic<int64_t>  min;
    std::atomic<int64_t>  max;
    std::atomic<int64_t>  last;
};

template <typename T, size_t N>
struct Sharded { T shards[N]; };

// Avoid zero-length C arrays when a registry has no entries of a kind.
template <size_t N>
inline constexpr size_t nz_v = (N == 0) ? 1 : N;

Sharded<TimerShard,   kShards> g_timers  [nz_v<kTimerCount>];
Sharded<CounterShard, kShards> g_counters[nz_v<kCounterCount>];
Sharded<ValueShard,   kShards> g_values  [nz_v<kValueCount>];

// ── Helpers ──────────────────────────────────────────────────────────
inline int current_shard() noexcept {
    const BaseType_t c = xPortGetCoreID();
    if (c < 0 || static_cast<size_t>(c) >= kShards) return 0;
    return static_cast<int>(c);
}

inline const Mapping& mapping_for(MetricId id) noexcept {
    return kMapping[static_cast<size_t>(id)];
}

void record_timer_sample(MetricId id, uint32_t duration_us) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Timer) return;
    auto& sh = g_timers[m.index].shards[current_shard()];
    sh.count.fetch_add(1,             std::memory_order_relaxed);
    sh.sum_us.fetch_add(duration_us,  std::memory_order_relaxed);
    sh.last_us.store(duration_us,     std::memory_order_relaxed);
    detail::atomic_min(sh.min_us, duration_us);
    detail::atomic_max(sh.max_us, duration_us);
}

}  // namespace

// ── init ─────────────────────────────────────────────────────────────
void init() noexcept {
    for (size_t i = 0; i < kTimerCount; ++i) {
        for (size_t s = 0; s < kShards; ++s) {
            auto& sh = g_timers[i].shards[s];
            sh.count.store(0,           std::memory_order_relaxed);
            sh.sum_us.store(0,          std::memory_order_relaxed);
            sh.min_us.store(UINT32_MAX, std::memory_order_relaxed);
            sh.max_us.store(0,          std::memory_order_relaxed);
            sh.last_us.store(0,         std::memory_order_relaxed);
        }
    }
    for (size_t i = 0; i < kCounterCount; ++i) {
        for (size_t s = 0; s < kShards; ++s) {
            g_counters[i].shards[s].value.store(0, std::memory_order_relaxed);
        }
    }
    for (size_t i = 0; i < kValueCount; ++i) {
        for (size_t s = 0; s < kShards; ++s) {
            auto& sh = g_values[i].shards[s];
            sh.count.store(0, std::memory_order_relaxed);
            sh.sum.store(0,   std::memory_order_relaxed);
            sh.min.store(std::numeric_limits<int64_t>::max(),
                         std::memory_order_relaxed);
            sh.max.store(std::numeric_limits<int64_t>::min(),
                         std::memory_order_relaxed);
            sh.last.store(0,  std::memory_order_relaxed);
        }
    }
}

// ── Counter API ──────────────────────────────────────────────────────
void counter_inc(MetricId id, uint64_t delta) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Counter) return;
    g_counters[m.index].shards[current_shard()].value.fetch_add(
        delta, std::memory_order_relaxed);
}

void counter_set(MetricId id, uint64_t value) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Counter) return;
    // Replace the aggregate: write the target into the current shard
    // and zero the others so read_counter returns `value` exactly.
    const size_t cur = static_cast<size_t>(current_shard());
    for (size_t s = 0; s < kShards; ++s) {
        g_counters[m.index].shards[s].value.store(
            s == cur ? value : 0, std::memory_order_relaxed);
    }
}

// ── Value API ────────────────────────────────────────────────────────
void value_record(MetricId id, int64_t sample) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Value) return;
    auto& sh = g_values[m.index].shards[current_shard()];
    sh.count.fetch_add(1,      std::memory_order_relaxed);
    sh.sum.fetch_add(sample,   std::memory_order_relaxed);
    sh.last.store(sample,      std::memory_order_relaxed);
    detail::atomic_min(sh.min, sample);
    detail::atomic_max(sh.max, sample);
}

// ── Timer API ────────────────────────────────────────────────────────
TimerToken timer_start(MetricId id) noexcept {
    TimerToken t;
    t.id       = id;
    t.start_us = esp_timer_get_time();
    t.active   = true;
    return t;
}

void timer_stop(TimerToken& token) noexcept {
    if (!token.active) return;
    const int64_t now = esp_timer_get_time();
    const int64_t dur = now - token.start_us;
    uint32_t d32 = 0;
    if (dur > 0) {
        const int64_t clamped = (dur > 0xFFFFFFFFLL) ? 0xFFFFFFFFLL : dur;
        d32 = static_cast<uint32_t>(clamped);
    }
    record_timer_sample(token.id, d32);
    token.active = false;
}

void timer_record(MetricId id, uint32_t duration_us) noexcept {
    record_timer_sample(id, duration_us);
}

TimerScope::TimerScope(MetricId id) noexcept : token_(timer_start(id)) {}
TimerScope::~TimerScope() noexcept           { timer_stop(token_); }

// ── Snapshot readers ─────────────────────────────────────────────────
bool read_timer(MetricId id, TimerSnapshot& out) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Timer) return false;
    uint64_t count = 0, sum = 0;
    uint32_t mn = UINT32_MAX, mx = 0, last = 0;
    bool had = false;
    for (size_t s = 0; s < kShards; ++s) {
        auto& sh = g_timers[m.index].shards[s];
        const uint64_t c = sh.count.load(std::memory_order_relaxed);
        if (c == 0) continue;
        count += c;
        sum   += sh.sum_us.load(std::memory_order_relaxed);
        const uint32_t smn = sh.min_us.load(std::memory_order_relaxed);
        const uint32_t smx = sh.max_us.load(std::memory_order_relaxed);
        if (smn < mn) mn = smn;
        if (smx > mx) mx = smx;
        // `last_us` across shards is racy; taking whichever wins the
        // iteration is fine for observability.
        last = sh.last_us.load(std::memory_order_relaxed);
        had  = true;
    }
    out.count    = count;
    out.sum_us   = sum;
    out.min_us   = had ? mn : 0;
    out.max_us   = mx;
    out.avg_us   = had ? static_cast<uint32_t>(sum / count) : 0;
    out.last_us  = last;
    out.has_data = had;
    return true;
}

bool read_counter(MetricId id, CounterSnapshot& out) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Counter) return false;
    uint64_t v = 0;
    for (size_t s = 0; s < kShards; ++s) {
        v += g_counters[m.index].shards[s].value.load(
            std::memory_order_relaxed);
    }
    out.value = v;
    return true;
}

// ── Memory snapshot ──────────────────────────────────────────────────
void update_memory_snapshot() noexcept {
#if CONFIG_METRICS_ENABLE_MEMORY_METRICS
    value_record(MetricId::METRIC_HEAP_FREE_BYTES,
                 static_cast<int64_t>(esp_get_free_heap_size()));
    value_record(MetricId::METRIC_HEAP_MIN_FREE_BYTES,
                 static_cast<int64_t>(esp_get_minimum_free_heap_size()));
    value_record(MetricId::METRIC_HEAP_LARGEST_FREE_BLOCK,
                 static_cast<int64_t>(
                     heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    value_record(MetricId::METRIC_HEAP_INTERNAL_FREE_BYTES,
                 static_cast<int64_t>(
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#if CONFIG_SPIRAM
    value_record(MetricId::METRIC_HEAP_SPIRAM_FREE_BYTES,
                 static_cast<int64_t>(
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#endif
#endif  // CONFIG_METRICS_ENABLE_MEMORY_METRICS
}

// ── Text dump ────────────────────────────────────────────────────────
size_t dump_text(char* buf, size_t buf_size) noexcept {
#if CONFIG_METRICS_ENABLE_TEXT_DUMP
    if (!buf || buf_size == 0) return 0;
    // Shared bounded writer (one implementation in metrics_internal.h —
    // formerly a third hand-rolled copy of the exporter logic, FINDINGS
    // §8 DUP). `append` keeps the call-site shape; truncation latches in
    // `w.truncated` and further writes are refused, buffer stays
    // NUL-terminated.
    detail::Writer w{buf, buf_size, 0, false};
    auto append = [&](const char* fmt, auto... args) {
        detail::wput(w, fmt, args...);
    };

    for (size_t i = 0; i < kMetricCount; ++i) {
        const auto& d = kDescriptors[i];
        switch (d.kind) {
            case MetricKind::Timer: {
                TimerSnapshot s{};
                read_timer(d.id, s);
                if (!s.has_data) {
                    append("%s => no samples\n", d.name);
                } else {
                    append("%s => calls: %llu, min: %uus, max: %uus, "
                             "avg: %uus, total: %lluus, last: %uus\n",
                             d.name,
                             static_cast<unsigned long long>(s.count),
                             static_cast<unsigned>(s.min_us),
                             static_cast<unsigned>(s.max_us),
                             static_cast<unsigned>(s.avg_us),
                             static_cast<unsigned long long>(s.sum_us),
                             static_cast<unsigned>(s.last_us));
                }
                break;
            }
            case MetricKind::Counter: {
                CounterSnapshot s{};
                read_counter(d.id, s);
                append("%s => %llu\n", d.name,
                         static_cast<unsigned long long>(s.value));
                break;
            }
            case MetricKind::Value: {
                ValueSnapshot s{};
                read_value(d.id, s);
                if (!s.has_data) {
                    append("%s => no samples\n", d.name);
                } else {
                    append("%s => count: %llu, min: %lld, max: %lld, "
                             "avg: %lld, last: %lld\n",
                             d.name,
                             static_cast<unsigned long long>(s.count),
                             static_cast<long long>(s.min),
                             static_cast<long long>(s.max),
                             static_cast<long long>(s.avg),
                             static_cast<long long>(s.last));
                }
                break;
            }
        }
    }
    return w.off;
#else
    (void)buf; (void)buf_size;
    return 0;
#endif
}

bool read_value(MetricId id, ValueSnapshot& out) noexcept {
    const auto& m = mapping_for(id);
    if (m.kind != MetricKind::Value) return false;
    uint64_t count = 0;
    int64_t  sum   = 0;
    int64_t  mn    = std::numeric_limits<int64_t>::max();
    int64_t  mx    = std::numeric_limits<int64_t>::min();
    int64_t  last  = 0;
    bool     had   = false;
    for (size_t s = 0; s < kShards; ++s) {
        auto& sh = g_values[m.index].shards[s];
        const uint64_t c = sh.count.load(std::memory_order_relaxed);
        if (c == 0) continue;
        count += c;
        sum   += sh.sum.load(std::memory_order_relaxed);
        const int64_t smn = sh.min.load(std::memory_order_relaxed);
        const int64_t smx = sh.max.load(std::memory_order_relaxed);
        if (smn < mn) mn = smn;
        if (smx > mx) mx = smx;
        last = sh.last.load(std::memory_order_relaxed);
        had  = true;
    }
    out.count    = count;
    out.sum      = sum;
    out.min      = had ? mn : 0;
    out.max      = had ? mx : 0;
    out.avg      = had ? sum / static_cast<int64_t>(count) : 0;
    out.last     = last;
    out.has_data = had;
    return true;
}

// ── Component-private accessors (see metrics_internal.h) ─────────────
namespace detail {
const MetricDescriptor* descriptors() noexcept      { return kDescriptors; }
size_t                  descriptor_count() noexcept { return kMetricCount; }
}  // namespace detail

}  // namespace metrics

#else   // ── Disabled-build stubs ─────────────────────────────────────

namespace metrics {

void init() noexcept {}
void counter_inc(MetricId, uint64_t) noexcept {}
void counter_set(MetricId, uint64_t) noexcept {}
void value_record(MetricId, int64_t) noexcept {}

TimerToken timer_start(MetricId id) noexcept {
    return { id, 0, false };
}
void timer_stop(TimerToken&) noexcept {}
void timer_record(MetricId, uint32_t) noexcept {}

TimerScope::TimerScope(MetricId id) noexcept : token_{id, 0, false} {}
TimerScope::~TimerScope() noexcept {}

bool read_timer(MetricId, TimerSnapshot& o) noexcept   { o = {}; return false; }
bool read_counter(MetricId, CounterSnapshot& o) noexcept { o = {}; return false; }
bool read_value(MetricId, ValueSnapshot& o) noexcept   { o = {}; return false; }
void update_memory_snapshot() noexcept {}
size_t dump_text(char*, size_t) noexcept { return 0; }

namespace detail {
const MetricDescriptor* descriptors() noexcept      { return nullptr; }
size_t                  descriptor_count() noexcept { return 0; }
}  // namespace detail

}  // namespace metrics

#endif  // CONFIG_METRICS_ENABLED
