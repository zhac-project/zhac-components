// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host shim for esp_timer. znp_state.cpp reads esp_timer_get_time() to stamp
// the reset-burst ring; the impl (host_stubs.cpp) advances a monotonic clock.
#pragma once
#include <cstdint>
int64_t esp_timer_get_time(void);
