// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test timer shim — handles are real and track running/stopped, but
// timers never fire.
#pragma once
#include "FreeRTOS.h"

typedef struct StubTimer* TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);

TimerHandle_t xTimerCreate(const char* name, TickType_t period,
                           UBaseType_t auto_reload, void* timer_id,
                           TimerCallbackFunction_t cb);
BaseType_t    xTimerStart(TimerHandle_t t, TickType_t ticks);
BaseType_t    xTimerReset(TimerHandle_t t, TickType_t ticks);
BaseType_t    xTimerStop(TimerHandle_t t, TickType_t ticks);
BaseType_t    xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t ticks);
void*         pvTimerGetTimerID(TimerHandle_t t);

// Test hook: whether the timer whose id is `timer_id` is running, and its period.
bool          stub_timer_running(void* timer_id, TickType_t* period);
