#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
void vTaskDelay(TickType_t);

// device_shadow spawns task_shadow via xTaskCreatePinnedToCore. Like the
// zap_store flush task, the shadow housekeeping task NEVER runs on the host —
// its debounce/occupancy sweep and deferred NVS writes are timer/queue driven
// and would loop forever. Returning pdPASS WITHOUT invoking the entry function
// keeps the harness single-threaded and deterministic.
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*,
                                                 uint32_t, void*, UBaseType_t,
                                                 TaskHandle_t*, BaseType_t) {
    return pdPASS;
}

// Free-running tick is not needed for the store/config/attr surface; the
// pipeline time-window logic (throttle/debounce) is characterized by calling
// the shadow_pipeline_* helpers directly with explicit timestamps. A fixed 0
// tick keeps upsert timestamps deterministic.
static inline TickType_t xTaskGetTickCount(void) { return 0; }
