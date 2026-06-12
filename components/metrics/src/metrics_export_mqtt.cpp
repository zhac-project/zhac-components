// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// metrics_export_mqtt.cpp — JSON snapshot formatter.
//
// Shape: one object at top level with three sub-objects (`timers`,
// `counters`, `values`). Each inner key is the raw metric name
// (without Kconfig prefix — the prefix is a Prometheus concern, MQTT
// topics already carry project scope).

#include "metrics/metrics_export_mqtt.h"
#include "metrics/metrics.h"
#include "metrics_internal.h"

#include "sdkconfig.h"

#if CONFIG_METRICS_ENABLED && CONFIG_METRICS_ENABLE_MQTT_EXPORTER

namespace metrics {

using detail::Writer;
using detail::wput;

size_t mqtt_format_snapshot_json(char* buf, size_t buf_size) noexcept {
    if (!buf || buf_size == 0) return 0;

    Writer w{buf, buf_size, 0, false};

    const auto*  desc  = detail::descriptors();
    const size_t total = detail::descriptor_count();

    wput(w, "{\"timers\":{");
    bool first = true;
    for (size_t i = 0; i < total; ++i) {
        if (desc[i].kind != MetricKind::Timer) continue;
        TimerSnapshot s{};
        read_timer(desc[i].id, s);
        wput(w,
             "%s\"%s\":{\"count\":%llu,\"sum_us\":%llu,\"min_us\":%u,"
             "\"max_us\":%u,\"avg_us\":%u,\"last_us\":%u}",
             first ? "" : ",", desc[i].name,
             static_cast<unsigned long long>(s.count),
             static_cast<unsigned long long>(s.sum_us),
             static_cast<unsigned>(s.min_us),
             static_cast<unsigned>(s.max_us),
             static_cast<unsigned>(s.avg_us),
             static_cast<unsigned>(s.last_us));
        first = false;
    }
    wput(w, "},\"counters\":{");
    first = true;
    for (size_t i = 0; i < total; ++i) {
        if (desc[i].kind != MetricKind::Counter) continue;
        CounterSnapshot s{};
        read_counter(desc[i].id, s);
        wput(w, "%s\"%s\":%llu",
             first ? "" : ",", desc[i].name,
             static_cast<unsigned long long>(s.value));
        first = false;
    }
    wput(w, "},\"values\":{");
    first = true;
    for (size_t i = 0; i < total; ++i) {
        if (desc[i].kind != MetricKind::Value) continue;
        ValueSnapshot s{};
        read_value(desc[i].id, s);
        wput(w,
             "%s\"%s\":{\"count\":%llu,\"sum\":%lld,\"min\":%lld,"
             "\"max\":%lld,\"avg\":%lld,\"last\":%lld}",
             first ? "" : ",", desc[i].name,
             static_cast<unsigned long long>(s.count),
             static_cast<long long>(s.sum),
             static_cast<long long>(s.min),
             static_cast<long long>(s.max),
             static_cast<long long>(s.avg),
             static_cast<long long>(s.last));
        first = false;
    }
    wput(w, "}}");

    // A truncated snapshot is missing closing braces (and possibly a
    // value mid-token) — i.e. syntactically invalid JSON. Publishing it
    // would feed garbage to every broker subscriber, so report 0 bytes
    // and let the caller skip the publish entirely rather than ship a
    // malformed document. The buffer stays NUL-terminated either way.
    // (FINDINGS §8 — truncation flag was never surfaced.)
    if (w.truncated) return 0;

    return w.off;
}

}  // namespace metrics

#else   // ── Disabled-build stub ─────────────────────────────────────

namespace metrics {
size_t mqtt_format_snapshot_json(char*, size_t) noexcept { return 0; }
}

#endif
