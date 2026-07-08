// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the FreeRTOS semaphore API, with REAL counting semantics — the
// znp_confirm ring depends on them:
//   * `while (xSemaphoreTake(sem, 0) == pdTRUE) {}` drains a slot on reserve.
//     A naive always-pdTRUE stub (as in device_shadow's harness) would spin
//     forever here, so binary semaphores must actually decrement to empty.
//   * A delivered AF_DATA_CONFIRM gives the slot semaphore; the waiter takes
//     it and reads the status. A real give/take is what makes the synchronous
//     confirm-delivery path testable without the RX task.
//
// Mutexes are modelled as always-available on this single-threaded harness
// (take/give never block or fail); binary semaphores are a 0/1 counter capped
// at 1 (FreeRTOS binary semantics). No blocking is possible on the host, so a
// take against an empty binary semaphore returns pdFALSE immediately (i.e. the
// timeout path) regardless of the requested tick timeout.
#pragma once
#include "FreeRTOS.h"

// Backing store for a statically-created semaphore. Must be a complete type:
// znp_confirm.cpp embeds one (`StaticSemaphore_t sem_buf`) per ring slot and
// hands its address to xSemaphoreCreate*Static().
typedef struct StaticSemaphore_t {
    int count;
    int is_mutex;
    int max_count;
} StaticSemaphore_t;

typedef void* SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buf);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* buf);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t sem);
