// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Minimal host shim for the FreeRTOS queue API used by device_shadow
// (task_shadow housekeeping queue) and the linked-real event_bus (per-
// subscriber FIFO). The shadow task never runs on the host (see task.h) and
// the tests install no subscribers, so every queue op is a benign no-op:
//   xQueueCreate  → non-null dummy handle
//   xQueueSend    → pdTRUE  (enqueue "succeeds"; nothing drains it)
//   xQueueReceive → pdFALSE (always empty; no drainer/task loops here)
//   vQueueDelete  → no-op
#pragma once
#include "FreeRTOS.h"

typedef void* QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) {
    static int s_queue_obj = 0;
    return &s_queue_obj;
}
static inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdFALSE; }
static inline void       vQueueDelete(QueueHandle_t) {}
