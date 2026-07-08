// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for esp_random(). zigbee_mgr uses it for a small amount of
// startup jitter / id salting; the characterization harness wants determinism,
// so this returns a fixed value. Header-only static inline: every TU that
// calls esp_random() resolves it internally, no link symbol required.
#pragma once
#include <cstdint>
#include <cstddef>
static inline uint32_t esp_random(void) { return 0x5A5A5A5Au; }
static inline void esp_fill_random(void* buf, size_t len) {
    for (size_t i = 0; i < len; i++) static_cast<uint8_t*>(buf)[i] = 0x5A;
}
