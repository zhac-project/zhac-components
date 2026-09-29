// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhac_task.h (included by zap_store_flush.cpp) declares its queue helpers
// against this API. zap_store makes no queue, so these are never called.
#pragma once
#include "FreeRTOS.h"

typedef void* QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) { return nullptr; }
static inline void          vQueueDelete(QueueHandle_t) {}
