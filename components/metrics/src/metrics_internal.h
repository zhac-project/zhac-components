// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Component-private accessors so other TUs in `components/metrics/`
// (exporters, dump, …) can iterate the descriptor table without each
// re-expanding the X-macro registry and duplicating the flash cost.

#include "metrics/metrics_types.h"
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace metrics::detail {

const MetricDescriptor* descriptors() noexcept;
size_t                  descriptor_count() noexcept;

// ── Bounded NUL-terminated buffer writer ─────────────────────────────
//
// Single implementation shared by all in-component formatters (MQTT
// JSON exporter, Prometheus text exporter, and the text dump). Each
// formerly carried its own verbatim copy / re-implementation (FINDINGS
// §8 DUP). `wput` refuses to write once the buffer is full, leaves the
// buffer NUL-terminated at `cap - 1`, and latches `truncated` so a
// caller can detect a partial (and therefore possibly malformed) result
// and decline to ship it rather than emit garbage.
struct Writer {
    char*  buf;
    size_t cap;
    size_t off;
    bool   truncated;
};

// `__attribute__((format))` keeps -Wformat coverage on the hoisted
// helper (the exporters build with -Werror). `inline` so the single
// definition can live in this shared header without an ODR clash.
inline void wput(Writer& w, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

inline void wput(Writer& w, const char* fmt, ...) {
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

}  // namespace metrics::detail
