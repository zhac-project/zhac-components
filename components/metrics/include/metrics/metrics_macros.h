// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Ergonomic macros. Each expands to a direct call into the metrics
// namespace when `CONFIG_METRICS_ENABLED=y`, and to a `do {...} while(0)`
// no-op (or a trivial stub) when disabled.
//
// The `_METRIC_START_TIMER` / `_METRIC_STOP_TIMER` pair takes an
// explicit token variable to keep the start/stop pairing lexical and
// avoid hidden TLS.

#include "sdkconfig.h"

#if CONFIG_METRICS_ENABLED

#include "metrics/metrics.h"

#define _METRIC_CONCAT2(a, b) a##b
#define _METRIC_CONCAT(a, b)  _METRIC_CONCAT2(a, b)

#define _METRIC_COUNTER_INC(id, delta) \
    ::metrics::counter_inc(::metrics::MetricId::id, (delta))

#define _METRIC_COUNTER_SET(id, value) \
    ::metrics::counter_set(::metrics::MetricId::id, (value))

#define _METRIC_VALUE(id, sample) \
    ::metrics::value_record(::metrics::MetricId::id, \
                              static_cast<int64_t>(sample))

#define _METRIC_START_TIMER(var, id) \
    auto var = ::metrics::timer_start(::metrics::MetricId::id)

#define _METRIC_STOP_TIMER(var) \
    ::metrics::timer_stop((var))

#define _METRIC_TIMER_SCOPE(id) \
    ::metrics::TimerScope _METRIC_CONCAT(_metric_scope_, __LINE__)(::metrics::MetricId::id)

// Memory + dump helpers — declarations land in their respective
// phases; macros are pre-declared here so users needn't touch them
// when those phases ship.
#define _METRIC_UPDATE_MEMORY_SNAPSHOT() \
    ::metrics::update_memory_snapshot()

#else  // !CONFIG_METRICS_ENABLED

#define _METRIC_COUNTER_INC(id, delta)    do { (void)(delta); } while (0)
#define _METRIC_COUNTER_SET(id, value)    do { (void)(value); } while (0)
#define _METRIC_VALUE(id, sample)         do { (void)(sample); } while (0)
#define _METRIC_START_TIMER(var, id)      int var = 0
#define _METRIC_STOP_TIMER(var)           do { (void)(var); } while (0)
#define _METRIC_TIMER_SCOPE(id)           do {} while (0)
#define _METRIC_UPDATE_MEMORY_SNAPSHOT()  do {} while (0)

#endif  // CONFIG_METRICS_ENABLED
