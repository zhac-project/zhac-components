// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// MQTT JSON snapshot formatter. Manual `snprintf`-based serializer;
// no cJSON, no heap. Caller owns the publisher schedule and the MQTT
// client binding.

#include "sdkconfig.h"
#include <cstddef>

namespace metrics {

// Serialise the full metric set into `buf` as a single JSON document:
//   { "timers": {...}, "counters": {...}, "values": {...} }
// Always NUL-terminated. Returns bytes populated excluding NUL, or 0
// on zero-size buffer / when CONFIG_METRICS_ENABLE_MQTT_EXPORTER=n.
size_t mqtt_format_snapshot_json(char* buf, size_t buf_size) noexcept;

}  // namespace metrics
