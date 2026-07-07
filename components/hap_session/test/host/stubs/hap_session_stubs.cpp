// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub implementations backing the hap_session characterization test:
//   • a controllable virtual clock for esp_timer_get_time() so retransmit /
//     ACK-timeout paths in hap_session_tick() are driven deterministically;
//   • no-op FreeRTOS mutex primitives (the host test is single-threaded, so the
//     session mutex only needs to be non-null and its take/give to succeed).
// The freertos/semphr.h and esp_timer.h stubs only DECLARE these — the bodies
// live here so exactly one translation unit defines them.
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ── Controllable virtual clock ───────────────────────────────────────────────
static int64_t g_now_us = 0;

int64_t esp_timer_get_time(void) { return g_now_us; }

void stub_clock_advance_ms(int64_t ms) { g_now_us += ms * 1000; }

void stub_clock_reset(void) { g_now_us = 0; }

// ── FreeRTOS mutex no-ops ────────────────────────────────────────────────────
// hap_session only requires the handle to be non-null (it guards every entry
// point on `if (!s_mutex)`), so hand back the address of a dummy object.
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    static int dummy_mutex;
    return &dummy_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }

BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
