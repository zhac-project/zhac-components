// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test task shim — xTaskCreate records the request but never runs the
// function (keeps task_cron's infinite loop out of the single-threaded test).
#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void*);
typedef struct StubTask* TaskHandle_t;

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                       void* arg, UBaseType_t priority, TaskHandle_t* out_handle);
void       vTaskDelay(TickType_t ticks);

// Task notifications (event_bus pump): no-ops on the single-threaded host.
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return nullptr; }
static inline void xTaskNotifyGive(TaskHandle_t) {}
static inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
