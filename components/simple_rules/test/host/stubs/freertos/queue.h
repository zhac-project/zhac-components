// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test queue shim — fixed-capacity FIFO ring, single-threaded.
#pragma once
#include "FreeRTOS.h"

typedef struct StubQueue* QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t    xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks);
BaseType_t    xQueueReceive(QueueHandle_t q, void* item, TickType_t ticks);
void          vQueueDelete(QueueHandle_t q);

// ── Test instrumentation (host shim only) ────────────────────────────────
// Without the hop-counter fix a self-feeding rule keeps the drain queue
// non-empty forever and event_bus_drain never returns. The budget converts
// that wedge into a detectable failure: once `n` receives have been served
// in the current phase, further receives on a NON-EMPTY queue set the
// runaway flag and return pdFALSE so the test process survives to report.
void stub_queue_set_receive_budget(unsigned long n);
bool stub_queue_runaway(void);

// Make the next xQueueCreate return nullptr (one-shot) — exercises the
// allocation-failure path in event_bus_subscribe. Default off.
void stub_queue_fail_next_create(void);
