// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// zhac_task.h — task and queue creation that parks the stack / storage in
// PSRAM where that is safe, and in internal DRAM everywhere else.
//
// Internal DRAM is the tight resource on every core (S3 dram0; the S31 wired
// build had 228 KB free at boot and 5 KB by the time mqtt_client tried to
// start its task). A task stack in PSRAM is only safe when the firmware
// keeps executing during flash erase/write, i.e. CONFIG_SPIRAM_XIP_FROM_PSRAM
// (code and rodata run from PSRAM, the cache stays on during MSPI1 flash
// operations). Builds without it get exactly the old xTaskCreate /
// xQueueCreate behaviour, so this header changes nothing on the dual-chip
// S3 + P4 or the mono S3 until they opt in.
//
// Queue storage in PSRAM is fine on the same condition; every caller here
// touches its queue from task context only, never from an ISR.
//
// C and C++ callers; host test harnesses see the plain FreeRTOS branch.
#pragma once
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
#define ZHAC_PSRAM_TASKS 1
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#define ZHAC_TASK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define ZHAC_PSRAM_TASKS 0
#endif

// Stack in PSRAM when ZHAC_PSRAM_TASKS, internal DRAM otherwise (also the
// fallback if PSRAM is somehow exhausted). A task created this way must not
// call vTaskDeleteWithCaps on itself; the few self-deleting endpoints in the
// tree just leak their stack once, which is documented at the call site.
static inline BaseType_t zhac_task_create(TaskFunction_t fn, const char* name, uint32_t stack_bytes,
                                          void* arg, UBaseType_t prio, TaskHandle_t* out) {
#if ZHAC_PSRAM_TASKS
    if (xTaskCreateWithCaps(fn, name, stack_bytes, arg, prio, out, ZHAC_TASK_CAPS) == pdPASS) return pdPASS;
#endif
    return xTaskCreate(fn, name, stack_bytes, arg, prio, out);
}

static inline BaseType_t zhac_task_create_pinned(TaskFunction_t fn, const char* name, uint32_t stack_bytes,
                                                 void* arg, UBaseType_t prio, TaskHandle_t* out,
                                                 BaseType_t core) {
#if ZHAC_PSRAM_TASKS
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, prio, out, core, ZHAC_TASK_CAPS) == pdPASS)
        return pdPASS;
#endif
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, prio, out, core);
}

// No internal fallback on purpose: a queue made WithCaps must be deleted
// WithCaps, so the pair below always agrees with itself.
static inline QueueHandle_t zhac_queue_create(UBaseType_t len, UBaseType_t item_bytes) {
#if ZHAC_PSRAM_TASKS
    return xQueueCreateWithCaps(len, item_bytes, ZHAC_TASK_CAPS);
#else
    return xQueueCreate(len, item_bytes);
#endif
}

static inline void zhac_queue_delete(QueueHandle_t q) {
#if ZHAC_PSRAM_TASKS
    vQueueDeleteWithCaps(q);
#else
    vQueueDelete(q);
#endif
}
