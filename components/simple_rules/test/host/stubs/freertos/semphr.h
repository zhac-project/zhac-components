// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test semaphore shim — single-threaded, take always succeeds.
#pragma once
#include "FreeRTOS.h"

typedef struct StubSemaphore* SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t        xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t        xSemaphoreGiveRecursive(SemaphoreHandle_t s);
