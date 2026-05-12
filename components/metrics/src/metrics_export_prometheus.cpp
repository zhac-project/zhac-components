// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// metrics_export_prometheus.cpp — Prometheus text formatter.
//
// Counters → native `counter`. Timers and value metrics → derived
// summary gauges suffixed with `_count` / `_sum` / `_min` / `_max` /
// `_avg` / `_last` (the spec opts out of native histograms/summaries
// for v1 because they require per-metric bucket arrays).

#include "metrics/metrics_export_prometheus.h"
#include "metrics/metrics.h"
#include "metrics_internal.h"

#include "sdkconfig.h"

#if CONFIG_METRICS_ENABLED && CONFIG_METRICS_ENABLE_PROMETHEUS_EXPORTER

#include <cstdarg>
#include <cstdio>

namespace metrics {
namespace {

struct Writer {
    char*  buf;
    size_t cap;
    size_t off;
    bool   truncated;
};

void wput(Writer& w, const char* fmt, ...) {
    if (w.truncated || w.off >= w.cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(w.buf + w.off, w.cap - w.off, fmt, ap);
    va_end(ap);
    if (n < 0) { w.truncated = true; return; }
    if (static_cast<size_t>(n) >= w.cap - w.off) {
        w.truncated = true;
        w.off = w.cap - 1;
        return;
    }
    w.off += static_cast<size_t>(n);
}

}  // namespace

size_t prometheus_format(char* buf, size_t buf_size,
                           const char* prefix) noexcept {
    if (!buf || buf_size == 0) return 0;
    if (!prefix || !*prefix) prefix = CONFIG_METRICS_EXPORT_PREFIX;

    Writer w{buf, buf_size, 0, false};

    const auto*  desc  = detail::descriptors();
    const size_t total = detail::descriptor_count();

    for (size_t i = 0; i < total; ++i) {
        const auto& d = desc[i];
        switch (d.kind) {
            case MetricKind::Counter: {
                CounterSnapshot s{};
                read_counter(d.id, s);
                wput(w, "# HELP %s_%s %s\n", prefix, d.name, d.help);
                wput(w, "# TYPE %s_%s counter\n", prefix, d.name);
                wput(w, "%s_%s %llu\n", prefix, d.name,
                     static_cast<unsigned long long>(s.value));
                break;
            }
            case MetricKind::Timer: {
                TimerSnapshot s{};
                read_timer(d.id, s);
                wput(w, "# HELP %s_%s_us %s\n",     prefix, d.name, d.help);
                wput(w, "# TYPE %s_%s_us_count counter\n", prefix, d.name);
                wput(w, "%s_%s_us_count %llu\n",    prefix, d.name,
                     static_cast<unsigned long long>(s.count));
                wput(w, "%s_%s_us_sum %llu\n",      prefix, d.name,
                     static_cast<unsigned long long>(s.sum_us));
                wput(w, "# TYPE %s_%s_us_min gauge\n", prefix, d.name);
                wput(w, "%s_%s_us_min %u\n",        prefix, d.name,
                     static_cast<unsigned>(s.min_us));
                wput(w, "# TYPE %s_%s_us_max gauge\n", prefix, d.name);
                wput(w, "%s_%s_us_max %u\n",        prefix, d.name,
                     static_cast<unsigned>(s.max_us));
                wput(w, "# TYPE %s_%s_us_avg gauge\n", prefix, d.name);
                wput(w, "%s_%s_us_avg %u\n",        prefix, d.name,
                     static_cast<unsigned>(s.avg_us));
                wput(w, "# TYPE %s_%s_us_last gauge\n", prefix, d.name);
                wput(w, "%s_%s_us_last %u\n",       prefix, d.name,
                     static_cast<unsigned>(s.last_us));
                break;
            }
            case MetricKind::Value: {
                ValueSnapshot s{};
                read_value(d.id, s);
                wput(w, "# HELP %s_%s %s\n",        prefix, d.name, d.help);
                wput(w, "# TYPE %s_%s_count counter\n", prefix, d.name);
                wput(w, "%s_%s_count %llu\n",       prefix, d.name,
                     static_cast<unsigned long long>(s.count));
                wput(w, "%s_%s_sum %lld\n",         prefix, d.name,
                     static_cast<long long>(s.sum));
                wput(w, "# TYPE %s_%s_min gauge\n", prefix, d.name);
                wput(w, "%s_%s_min %lld\n",         prefix, d.name,
                     static_cast<long long>(s.min));
                wput(w, "# TYPE %s_%s_max gauge\n", prefix, d.name);
                wput(w, "%s_%s_max %lld\n",         prefix, d.name,
                     static_cast<long long>(s.max));
                wput(w, "# TYPE %s_%s_avg gauge\n", prefix, d.name);
                wput(w, "%s_%s_avg %lld\n",         prefix, d.name,
                     static_cast<long long>(s.avg));
                wput(w, "# TYPE %s_%s_last gauge\n", prefix, d.name);
                wput(w, "%s_%s_last %lld\n",        prefix, d.name,
                     static_cast<long long>(s.last));
                break;
            }
        }
    }
    return w.off;
}

}  // namespace metrics

#else   // ── Disabled-build stub ─────────────────────────────────────

namespace metrics {

size_t prometheus_format(char*, size_t, const char*) noexcept {
    return 0;
}

}  // namespace metrics

#endif
