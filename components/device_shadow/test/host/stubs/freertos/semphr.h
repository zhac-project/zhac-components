#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);

// event_bus (linked real) uses a recursive mutex around its subscriber table.
// Single-threaded host harness: create returns a non-null dummy, take/give are
// no-ops. (device_shadow itself uses only the plain mutex above.)
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    static int s_recursive_obj = 0;
    return &s_recursive_obj;
}
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
