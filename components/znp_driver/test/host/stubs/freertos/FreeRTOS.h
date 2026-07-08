// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the FreeRTOS base surface used by the znp_driver transport
// TUs under test (znp_parser / znp_areq_dispatch / znp_confirm / znp_state).
//
// Base typedefs + macros are declared FIRST, then the sub-headers (task/queue/
// semphr) are umbrella-included at the bottom so a TU that includes only
// "freertos/FreeRTOS.h" still sees SemaphoreHandle_t / QueueHandle_t / the task
// helpers — matching ESP-IDF's aggregation. Placed last to avoid an include
// cycle (each sub-header re-includes this one under #pragma once).
#pragma once
#include <cstdint>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFF)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configMINIMAL_STACK_SIZE 768
#define configASSERT(x) ((void)0)
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1

// Critical-section spinlock. znp_state.cpp guards its state/stats behind a
// portMUX_TYPE. Single-threaded host harness: the type is a placeholder and
// enter/exit are no-ops that still touch the mux argument so it isn't flagged
// unused.
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux)     ((void)(mux))
#define portEXIT_CRITICAL(mux)      ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)  ((void)(mux))

// Umbrella aggregation (declared last, after the base typedefs above).
#include "task.h"
#include "queue.h"
#include "semphr.h"
