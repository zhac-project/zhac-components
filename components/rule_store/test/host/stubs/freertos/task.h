#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);   // zhac_task.h spells its parameters with this
BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
void vTaskDelay(TickType_t);
// zhac_task.h's pinned helper; rule_store never pins a task.
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*,
                                                 UBaseType_t, TaskHandle_t*, BaseType_t) {
    return pdPASS;
}
