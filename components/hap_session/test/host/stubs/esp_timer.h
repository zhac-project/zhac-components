// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub for esp_timer with a CONTROLLABLE monotonic clock so the retransmit
// / ACK-timeout logic in hap_session_tick() can be driven deterministically
// (no real sleeps). The production code reads esp_timer_get_time() (µs since
// boot) and divides by 1000 for its ms bookkeeping. Here the clock only ever
// moves when the test calls stub_clock_advance_ms(); it never advances on its
// own, so a tick before the timeout and a tick after it are fully reproducible.
#pragma once
#include <cstdint>

// Monotonic microseconds since (virtual) boot. Returns whatever the test has
// advanced the clock to; starts at 0.
int64_t esp_timer_get_time(void);

// Test-only clock control (implemented in hap_session_stubs.cpp).
void stub_clock_advance_ms(int64_t ms);   // add ms*1000 to the virtual clock
void stub_clock_reset(void);              // reset the virtual clock back to 0
