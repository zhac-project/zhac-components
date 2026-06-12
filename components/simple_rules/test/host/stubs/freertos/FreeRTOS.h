// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host-test FreeRTOS shim — single-threaded stand-ins for the queue /
// semaphore / timer / task primitives simple_rules + event_bus touch.
// Blocking semantics collapse to "immediate": a tick argument is ignored,
// an empty queue returns pdFALSE right away. Good enough to exercise the
// dispatch logic deterministically on the build host.
#pragma once
#include <stdint.h>
#include <assert.h>

typedef int          BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t     TickType_t;

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  pdTRUE

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY     ((TickType_t)0xFFFFFFFFu)

#define configASSERT(x) assert(x)
