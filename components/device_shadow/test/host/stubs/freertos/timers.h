// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Minimal host shim for the FreeRTOS software-timer API used by device_shadow
// for the per-device debounce and occupancy-TTL timers. On the host the timer
// service task does not exist, so timers never FIRE — the debounce-flush and
// occupancy=0 synthesis they would trigger run on task_shadow, which is also
// stubbed out (see task.h). The tests therefore characterize the arm/disarm
// bookkeeping (a non-null handle is created, stored, and deleted) rather than
// callback delivery, which is documented as not host-testable.
//   xTimerCreate        → non-null dummy handle (so the create-guard succeeds)
//   xTimerReset/Change  → pdPASS
//   xTimerIsTimerActive → pdFALSE (never armed from the host's perspective)
//   pvTimerGetTimerID   → nullptr (only read inside never-firing callbacks)
//   vTimerSetTimerID    → no-op (relocation fixup in device_shadow_remove)
#pragma once
#include "FreeRTOS.h"

typedef void* TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

static inline TimerHandle_t xTimerCreate(const char*, TickType_t, UBaseType_t,
                                         void*, TimerCallbackFunction_t) {
    static int s_timer_obj = 0;
    return &s_timer_obj;
}
static inline BaseType_t xTimerReset(TimerHandle_t, TickType_t) { return pdPASS; }
static inline BaseType_t xTimerChangePeriod(TimerHandle_t, TickType_t, TickType_t) { return pdPASS; }
static inline BaseType_t xTimerDelete(TimerHandle_t, TickType_t) { return pdPASS; }
static inline BaseType_t xTimerIsTimerActive(TimerHandle_t) { return pdFALSE; }
static inline void*      pvTimerGetTimerID(TimerHandle_t) { return nullptr; }
static inline void       vTimerSetTimerID(TimerHandle_t, void*) {}
