// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sys_tasks — every FreeRTOS task with its CPU share, core, priority and stack
// headroom, for the Diag page ("which task eats core 0?"). Header-only, like
// sys_metrics.h. Call it from ONE place: the share is measured between two
// consecutive calls (the run-time counters are 32-bit and wrap after ~71
// minutes, so "since boot" would lie on a hub with days of uptime), and the
// baseline lives in a function-local static.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

struct SysTaskRow {
    char     name[configMAX_TASK_NAME_LEN];
    int8_t   core;        // pinned core, -1 = any
    uint8_t  cpu_pct;     // share of wall time since the previous call, 0-100 per task
    uint8_t  prio;
    uint32_t stack_free;  // bytes of stack never used
};

// Fills up to `cap` rows, highest CPU share first. The first call after boot
// reports 0 % everywhere (no interval yet). Returns the row count; 0 when the
// build lacks run-time stats or the trace facility.
static inline size_t sys_tasks_snapshot(SysTaskRow* out, size_t cap) {
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    constexpr size_t kMaxTracked = 64;
    struct Base { TaskHandle_t h; uint32_t run; };
    static Base     s_base[kMaxTracked];
    static size_t   s_base_n = 0;
    static uint32_t s_last_total = 0;
    static bool     s_seeded = false;

    const UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0 || !out || cap == 0) return 0;
    auto* arr = static_cast<TaskStatus_t*>(calloc(n, sizeof(TaskStatus_t)));
    if (!arr) return 0;
    uint32_t total = 0;
    const UBaseType_t got = uxTaskGetSystemState(arr, n, &total);
    const uint32_t dt = s_seeded ? (total - s_last_total) : 0;   // wrap-safe subtraction

    size_t count = 0;
    for (UBaseType_t i = 0; i < got && count < cap; i++) {
        SysTaskRow& r = out[count++];
        snprintf(r.name, sizeof(r.name), "%s", arr[i].pcTaskName ? arr[i].pcTaskName : "?");
        r.prio       = static_cast<uint8_t>(arr[i].uxCurrentPriority);
        r.stack_free = static_cast<uint32_t>(arr[i].usStackHighWaterMark) * sizeof(StackType_t);
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        r.core = (arr[i].xCoreID == tskNO_AFFINITY) ? -1 : static_cast<int8_t>(arr[i].xCoreID);
#else
        r.core = -1;
#endif
        r.cpu_pct = 0;
        if (dt > 0) {
            for (size_t b = 0; b < s_base_n; b++) {
                if (s_base[b].h != arr[i].xHandle) continue;
                const uint32_t d = arr[i].ulRunTimeCounter - s_base[b].run;
                uint64_t pct = static_cast<uint64_t>(d) * 100 / dt;
                r.cpu_pct = static_cast<uint8_t>(pct > 100 ? 100 : pct);
                break;
            }
        }
    }
    // New baseline: handle + counter for every task seen (a recycled handle
    // mis-attributes one interval; nothing to do about that cheaply).
    s_base_n = 0;
    for (UBaseType_t i = 0; i < got && s_base_n < kMaxTracked; i++) {
        s_base[s_base_n++] = Base{arr[i].xHandle, static_cast<uint32_t>(arr[i].ulRunTimeCounter)};
    }
    s_last_total = total;
    s_seeded = true;
    free(arr);

    // Highest share first; ties keep the kernel's order.
    for (size_t i = 1; i < count; i++) {
        SysTaskRow key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].cpu_pct < key.cpu_pct) { out[j] = out[j - 1]; j--; }
        out[j] = key;
    }
    return count;
#else
    (void)out; (void)cap;
    return 0;
#endif
}
