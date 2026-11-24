#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);   // zhac_task.h spells its parameters with this
BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
void vTaskDelay(TickType_t);
// event_bus's notify-driven pump (event_bus_pump_run) compiles in this harness
// too; the pump never runs on the host, so these are inert like the simple_rules shim.
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return nullptr; }
static inline void xTaskNotifyGive(TaskHandle_t) {}
static inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }

// device_shadow spawns task_shadow via xTaskCreatePinnedToCore. Like the
// zap_store flush task, the shadow housekeeping task does not run on its own
// on the host — its debounce/occupancy sweep and deferred NVS writes are
// timer/queue driven and would loop forever. Returning pdPASS WITHOUT invoking
// the entry function keeps the harness single-threaded and deterministic; the
// entry function is kept, so a test can run one sweep of it (queue.h
// g_stub_queue_receives ends the loop).
inline void (*g_stub_pinned_task)(void*) = nullptr;
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*,
                                                 uint32_t, void*, UBaseType_t,
                                                 TaskHandle_t*, BaseType_t) {
    g_stub_pinned_task = fn;
    return pdPASS;
}

// Free-running tick is not needed for the store/config/attr surface; the
// pipeline time-window logic (throttle/debounce) is characterized by calling
// the shadow_pipeline_* helpers directly with explicit timestamps. A fixed 0
// tick keeps upsert timestamps deterministic.
static inline TickType_t xTaskGetTickCount(void) { return 0; }
