#pragma once
#include <cstdint>
// Host shim: the test moves the clock by hand.
extern int64_t g_fake_time_us;
static inline int64_t esp_timer_get_time(void) { return g_fake_time_us; }
