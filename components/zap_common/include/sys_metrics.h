// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared CPU-percentage sampler for P4 heartbeat + S3 /api/status.
// Both chips run the same calculation: each call samples FreeRTOS
// per-core idle run-time counters, deltas against a baseline the CALLER
// owns, and returns 0-100 busy percent per core.
//
// Baseline ownership (FINDINGS §8 fix). This is a header function; a
// function-local `static` here is per-translation-unit, NOT
// per-call-site as the old doc claimed — two call sites in one TU (or
// two tasks racing the same TU's copy) would share and corrupt one
// rolling window, with no synchronisation, yielding bogus CPU%. The
// baseline now lives in a caller-supplied `sys_metrics_cpu_ctx_t`:
//   - each measurement cadence (heartbeat task, REST handler, …) keeps
//     its own zero-initialised ctx, so windows never cross;
//   - a ctx is single-owner state — guard it like any other per-context
//     scratch if more than one task can call with the same ctx (the
//     in-tree callers each own a private file-scope ctx touched by one
//     task, so no extra lock is needed there).
// Zero-initialise the ctx (`{}` / `= {0}`) before the first call.
//
// Output is zero on the first call (no baseline) and whenever
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is off.

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// Caller-owned rolling-window baseline. `seeded` distinguishes a
// genuine zero baseline from "never sampled" so the first call reports
// 0 rather than a garbage delta.
struct sys_metrics_cpu_ctx_t {
    uint32_t last_tm;
    uint32_t last_idle0;
    uint32_t last_idle1;
    bool     seeded;
};

static inline void sys_metrics_sample_cpu_pct(sys_metrics_cpu_ctx_t& ctx,
                                              uint8_t& c0, uint8_t& c1) {
    c0 = 0; c1 = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
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
    if (ctx.seeded) {
        uint32_t dt = now_tm - ctx.last_tm;
        uint32_t d0 = cur0   - ctx.last_idle0;
        uint32_t d1 = cur1   - ctx.last_idle1;
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
    ctx.last_tm    = now_tm;
    ctx.last_idle0 = cur0;
    ctx.last_idle1 = cur1;
    ctx.seeded     = true;
#else
    (void)ctx;
#endif
}
