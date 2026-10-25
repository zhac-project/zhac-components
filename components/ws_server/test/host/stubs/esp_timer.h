// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Host stub of esp_timer: the clock is the test's (g_now_us); timers never fire,
// the test calls what they would.
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK 0
typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void*);
typedef enum { ESP_TIMER_TASK } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback; void* arg; esp_timer_dispatch_t dispatch_method; const char* name; bool skip_unhandled_events;
} esp_timer_create_args_t;

inline int64_t g_now_us = 0;
inline int64_t esp_timer_get_time() { return g_now_us; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* h) {
    *h = reinterpret_cast<esp_timer_handle_t>(1);
    return ESP_OK;
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return ESP_OK; }
