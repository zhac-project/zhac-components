// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the ESP-IDF UART driver. znp_internal.h (pulled in by every
// transport TU) declares `extern uart_port_t znp_uart_port`, so the port type
// must exist even though the units under test — the MT codec, AREQ dispatch,
// confirm ring, and state machine — never touch UART.
//
// The full recording surface (install/param/pin/write/read/flush + constants
// and event types) is provided anyway so this harness stays a drop-in should
// znp_transport.cpp / znp_rx.cpp later be linked in: TX bytes are captured into
// g_uart_tx and RX can be fed from g_uart_rx. Definitions live in
// host_stubs.cpp (external linkage → no unused-symbol lint under -Werror).
#pragma once
#include "esp_err.h"
#include "freertos/queue.h"
#include <cstddef>
#include <cstdint>

// ── Port + config types ───────────────────────────────────────────────────
typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

// Field values are placeholders; no compiled TU inspects them on the host.
#define UART_DATA_8_BITS        3
#define UART_PARITY_DISABLE     0
#define UART_STOP_BITS_1        1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT       0
#define UART_PIN_NO_CHANGE      (-1)

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int rx_flow_ctrl_thresh;
    int source_clk;
} uart_config_t;

// ── UART event queue surface (used by the out-of-scope RX task) ────────────
typedef enum {
    UART_DATA,
    UART_BREAK,
    UART_BUFFER_FULL,
    UART_FIFO_OVF,
    UART_FRAME_ERR,
    UART_PARITY_ERR,
    UART_PATTERN_DET,
    UART_EVENT_MAX,
} uart_event_type_t;

typedef struct {
    uart_event_type_t type;
    size_t            size;
} uart_event_t;

// ── Driver API (recording impls in host_stubs.cpp) ─────────────────────────
esp_err_t uart_driver_install(uart_port_t port, int rx_buf, int tx_buf,
                              int queue_size, QueueHandle_t* out_queue,
                              int intr_flags);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t* cfg);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
int       uart_write_bytes(uart_port_t port, const void* src, size_t size);
int       uart_read_bytes(uart_port_t port, void* buf, uint32_t length,
                          TickType_t ticks_to_wait);
esp_err_t uart_flush_input(uart_port_t port);

// ── Test hooks (external linkage; defined in host_stubs.cpp) ───────────────
// Last TX capture.
extern uint8_t g_uart_tx[512];
extern size_t  g_uart_tx_len;
// Injectable RX: point g_uart_rx / g_uart_rx_len at bytes to hand back from
// uart_read_bytes (advances g_uart_rx_pos as consumed).
extern const uint8_t* g_uart_rx;
extern size_t         g_uart_rx_len;
extern size_t         g_uart_rx_pos;
