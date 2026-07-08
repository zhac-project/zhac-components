// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Out-of-line host stub implementations for the znp_driver characterization
// harness: a monotonic clock, a no-op task delay, real counting semaphores,
// and a recording UART surface. See the corresponding headers for rationale.

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#include <cstring>

// ── esp_timer ──────────────────────────────────────────────────────────────
// Monotonic microsecond clock; +1 ms per call keeps znp_state's reset-burst
// timestamps distinct and ordered without any wall-clock dependency.
int64_t esp_timer_get_time(void) {
    static int64_t s_now_us = 0;
    s_now_us += 1000;
    return s_now_us;
}

// ── task ────────────────────────────────────────────────────────────────────
void vTaskDelay(TickType_t) { /* single-threaded host: nothing to yield to */ }

// ── semaphores (real 0/1 counting; mutexes always available) ────────────────
namespace {
inline BaseType_t sem_take(StaticSemaphore_t* s) {
    if (!s) return pdFALSE;
    if (s->is_mutex) return pdTRUE;              // no contention on the host
    if (s->count > 0) { s->count--; return pdTRUE; }
    return pdFALSE;                              // empty binary → timeout path
}
inline BaseType_t sem_give(StaticSemaphore_t* s) {
    if (!s) return pdFALSE;
    if (s->is_mutex) return pdTRUE;
    if (s->count < s->max_count) s->count++;     // FreeRTOS binary caps at 1
    return pdTRUE;
}
}  // namespace

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    // znp_areq_dispatch creates this once at C++ static-init. A function-local
    // static is constructed on first call (here), so there is no static-init
    // ordering hazard with the caller's own static.
    static StaticSemaphore_t s_dyn_mutex{1, 1, 1};
    return &s_dyn_mutex;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buf) {
    buf->count = 1; buf->is_mutex = 1; buf->max_count = 1;
    return buf;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* buf) {
    buf->count = 0; buf->is_mutex = 0; buf->max_count = 1;  // starts empty
    return buf;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
    return sem_take(static_cast<StaticSemaphore_t*>(sem));
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    return sem_give(static_cast<StaticSemaphore_t*>(sem));
}

// ── recording UART (unused by the compiled TUs; ready for transport tests) ──
uint8_t       g_uart_tx[512]  = {};
size_t        g_uart_tx_len   = 0;
const uint8_t* g_uart_rx      = nullptr;
size_t        g_uart_rx_len   = 0;
size_t        g_uart_rx_pos   = 0;

esp_err_t uart_driver_install(uart_port_t, int, int, int,
                              QueueHandle_t* out_queue, int) {
    if (out_queue) *out_queue = nullptr;
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t, const uart_config_t*) { return ESP_OK; }
esp_err_t uart_set_pin(uart_port_t, int, int, int, int)        { return ESP_OK; }

int uart_write_bytes(uart_port_t, const void* src, size_t size) {
    size_t n = size < sizeof(g_uart_tx) ? size : sizeof(g_uart_tx);
    if (src && n) memcpy(g_uart_tx, src, n);
    g_uart_tx_len = n;
    return static_cast<int>(size);
}
int uart_read_bytes(uart_port_t, void* buf, uint32_t length, TickType_t) {
    size_t avail = g_uart_rx_len - g_uart_rx_pos;
    size_t n = length < avail ? length : avail;
    if (buf && n) memcpy(buf, g_uart_rx + g_uart_rx_pos, n);
    g_uart_rx_pos += n;
    return static_cast<int>(n);
}
esp_err_t uart_flush_input(uart_port_t) { return ESP_OK; }
