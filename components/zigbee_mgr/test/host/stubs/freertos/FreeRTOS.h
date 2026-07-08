#pragma once
#include <cstdint>
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFF)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configMINIMAL_STACK_SIZE 768
#define configASSERT(x) ((void)0)
// device_shadow derives seconds/milliseconds from the tick count.
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
// 1:1 tick<->ms (configTICK_RATE_HZ == 1000). zigbee_interview uses this for
// a shaved-wait log computation.
#define pdTICKS_TO_MS(t) ((TickType_t)(t))

// Critical-section spinlock. zigbee_configure_queue guards its retry table with
// a portMUX_TYPE. Single-threaded host harness: the type is a placeholder and
// enter/exit are no-ops that still touch the mux argument so it isn't flagged
// unused.
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux)     ((void)(mux))
#define portEXIT_CRITICAL(mux)      ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)  ((void)(mux))

// Umbrella aggregation. zigbee_mgr's TUs (e.g. zigbee_interview.cpp) assume the
// full ESP-IDF FreeRTOS surface is visible after including only
// "freertos/FreeRTOS.h" (SemaphoreHandle_t, queue/timer types, …). The base
// typedefs/macros above are declared FIRST so the sub-headers — each of which
// re-includes this file under #pragma once — see them. Placed last to avoid an
// include cycle.
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
