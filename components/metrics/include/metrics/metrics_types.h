// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Public types for the metrics engine. The `MetricId` enum is generated
// from `metric_registry.def` via X-macros; every other public surface
// (counter/value/timer APIs, snapshots) is hand-written.
//
// `sdkconfig.h` is pulled in so conditional entries in the registry
// (e.g. SPIRAM metrics gated on CONFIG_SPIRAM) resolve the same way
// here as in `metrics.cpp`.

#include "sdkconfig.h"

#include <cstddef>
#include <cstdint>

namespace metrics {

enum class MetricKind : uint8_t {
    Timer,
    Counter,
    Value,
};

enum class MetricId : uint16_t {
#define METRIC_TIMER(id, name, help)   id,
#define METRIC_COUNTER(id, name, help) id,
#define METRIC_VALUE(id, name, help)   id,
#include "metrics/metric_registry.def"
#undef METRIC_TIMER
#undef METRIC_COUNTER
#undef METRIC_VALUE
    _COUNT
};

struct MetricDescriptor {
    MetricId     id;
    MetricKind   kind;
    const char*  name;
    const char*  help;
};

struct TimerSnapshot {
    uint64_t count;
    uint64_t sum_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t last_us;
    bool     has_data;
};

struct CounterSnapshot {
    uint64_t value;
};

struct ValueSnapshot {
    uint64_t count;
    int64_t  sum;
    int64_t  min;
    int64_t  max;
    int64_t  avg;
    int64_t  last;
    bool     has_data;
};

struct TimerToken {
    MetricId id;
    int64_t  start_us;
    bool     active;
};

}  // namespace metrics
