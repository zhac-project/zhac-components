// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared CPU-percentage sampler for P4 heartbeat + S3 /api/status.
// Both chips run the same calculation: each call samples FreeRTOS
// per-core idle run-time counters, deltas against its own per-call-site
// static baseline, and returns 0-100 busy percent per core.
//
// The per-call-site static baseline means each distinct inclusion gets
// its own rolling window — don't share windows between the heartbeat
// path and a REST handler that fires on different cadences.
//
// Output is zero on the first call (no baseline) and whenever
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is off.

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static inline void sys_metrics_sample_cpu_pct(uint8_t& c0, uint8_t& c1) {
    c0 = 0; c1 = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static uint32_t s_last_tm    = 0;
    static uint32_t s_last_idle0 = 0;
    static uint32_t s_last_idle1 = 0;
    auto read_run_counter = [](TaskHandle_t h) -> uint32_t {
        if (!h) return 0;
        TaskStatus_t st{};
        vTaskGetInfo(h, &st, pdFALSE, eInvalid);
        return (uint32_t)st.ulRunTimeCounter;
    };
    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle1 = (portNUM_PROCESSORS > 1)
                         ? xTaskGetIdleTaskHandleForCore(1) : nullptr;
    uint32_t now_tm = (uint32_t)portGET_RUN_TIME_COUNTER_VALUE();
    uint32_t cur0   = read_run_counter(idle0);
    uint32_t cur1   = read_run_counter(idle1);
    if (s_last_tm != 0) {
        uint32_t dt = now_tm - s_last_tm;
        uint32_t d0 = cur0   - s_last_idle0;
        uint32_t d1 = cur1   - s_last_idle1;
        if (dt > 0) {
            int p0 = 100 - (int)((uint64_t)d0 * 100 / dt);
            int p1 = idle1 ? (100 - (int)((uint64_t)d1 * 100 / dt)) : 0;
            if (p0 < 0) p0 = 0;
            if (p0 > 100) p0 = 100;
            if (p1 < 0) p1 = 0;
            if (p1 > 100) p1 = 100;
            c0 = (uint8_t)p0;
            c1 = (uint8_t)p1;
        }
    }
    s_last_tm    = now_tm;
    s_last_idle0 = cur0;
    s_last_idle1 = cur1;
#endif
}
