// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host stub for the FreeRTOS task API surface used by sys_metrics.h.
// Idle-task run-time counters are driven by test globals so the
// per-core CPU% delta can be scripted off-target.
#pragma once

#include <cstdint>

typedef void* TaskHandle_t;

typedef enum { eInvalid = 0 } eTaskState;

// Per-core idle-task run-time counters, set by the test before a sample.
extern uint32_t g_stub_idle0_counter;
extern uint32_t g_stub_idle1_counter;

typedef struct {
    uint32_t ulRunTimeCounter;
} TaskStatus_t;

// Stable, distinct, non-null per-core handles (addresses of function
// statics) so the sampler's `if (!h)` guard sees both cores present and
// vTaskGetInfo can route by handle identity.
static inline TaskHandle_t xTaskGetIdleTaskHandleForCore(int core) {
    static int s_core0;
    static int s_core1;
    return core == 0 ? (TaskHandle_t)&s_core0 : (TaskHandle_t)&s_core1;
}

static inline void vTaskGetInfo(TaskHandle_t h, TaskStatus_t* st,
                                int /*getFreeStack*/, eTaskState /*state*/) {
    st->ulRunTimeCounter = (h == xTaskGetIdleTaskHandleForCore(0))
                           ? g_stub_idle0_counter
                           : g_stub_idle1_counter;
}
