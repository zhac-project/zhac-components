// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Prometheus text exposition formatter. Core engine is
// transport-agnostic; HTTP glue lives in the consuming firmware
// (see `zhac-net-core/main/rest_prom.cpp`).

#include "sdkconfig.h"
#include <cstddef>

namespace metrics {

// Serialise the full metric set into `buf` in Prometheus text
// exposition format. Output is always NUL-terminated when
// `buf_size > 0`. Returns bytes populated (excluding NUL). Output is
// capped at `buf_size - 1` on truncation; there is no chunking.
//
// `prefix` is prepended to every metric name with an underscore
// separator. Pass nullptr (or empty) to use `CONFIG_METRICS_EXPORT_PREFIX`.
//
// When `CONFIG_METRICS_ENABLE_PROMETHEUS_EXPORTER=n` this function
// exists as a zero-emit stub so consumers can call it unconditionally.
size_t prometheus_format(char* buf, size_t buf_size,
                           const char* prefix = nullptr) noexcept;

}  // namespace metrics
