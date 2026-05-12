// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_transport.cpp — bring-up: UART config, reset/BSL GPIOs, spawn tasks.
//
// Architecture block:
//
//   caller task                       worker task                 rx task
//   ───────────                       ───────────                 ───────
//   znp_call() ──┐                                               read uart
//                └► s_request_q ─► dequeue ─► encode ─► TX       ─► parser
//                                               │                   │
//                                               ▼                   ▼
//                                            wait wake_q     on_frame() classifies
//                                               ▲                │
//                                   deliver_srsp / reset_ind ◄───┤
//                                                                │
//                                   znp_areq_dispatch() ◄────────┘
//                      reply_queue (per call)
//                                ▲
//                                └─── send ZnpReply
//
// No global shared SRSP buffer. No two-phase mutex+semaphore. Every piece
// of cross-task state is a FreeRTOS queue carrying owning ZnpFrame values.

#include "znp_internal.h"
#include "znp_transport.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "znp_transport";

uart_port_t znp_uart_port = UART_NUM_1;

#ifdef CONFIG_ZHAC_ZNP_UART_TX_GPIO
static constexpr gpio_num_t PIN_TX     = (gpio_num_t)CONFIG_ZHAC_ZNP_UART_TX_GPIO;
static constexpr gpio_num_t PIN_RX     = (gpio_num_t)CONFIG_ZHAC_ZNP_UART_RX_GPIO;
static constexpr gpio_num_t PIN_NRESET = (gpio_num_t)CONFIG_ZHAC_ZNP_UART_RST_GPIO;
static constexpr gpio_num_t PIN_BSL    = (gpio_num_t)CONFIG_ZHAC_ZNP_UART_BSL_GPIO;
#else
static constexpr gpio_num_t PIN_TX     = GPIO_NUM_16;
static constexpr gpio_num_t PIN_RX     = GPIO_NUM_17;
static constexpr gpio_num_t PIN_NRESET = GPIO_NUM_28;
static constexpr gpio_num_t PIN_BSL    = GPIO_NUM_29;
#endif

static constexpr int UART_BUF_SZ = 4096;

static bool s_started = false;

static void init_uart() {
    uart_config_t cfg = {
        .baud_rate           = 115200,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
        .flags               = {},
    };
    ESP_ERROR_CHECK(uart_param_config(znp_uart_port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(znp_uart_port, PIN_TX, PIN_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(znp_uart_port, UART_BUF_SZ, 0, 0,
                                         nullptr, 0));
}

static void init_gpios() {
    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << PIN_NRESET) | (1ULL << PIN_BSL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&gc));
    gpio_set_level(PIN_NRESET, 1);
    // BSL HIGH = inactive (normal boot). Koenkk coordinator firmware for
    // ebyte/LaunchPad modules enables DIO_15 BSL sampling active-low in
    // CCFG: LOW at reset = ROM bootloader, HIGH = run application. The
    // future in-ZHAC flasher pulses this LOW then toggles nRST to enter
    // BSL on demand. See zhac-main-core/README.md for details.
    gpio_set_level(PIN_BSL, 1);
}

void znp_transport_start() {
    if (s_started) return;
    s_started = true;

    znp_state_set(ZnpTransportState::Booting);
    init_uart();
    init_gpios();
    znp_rx_task_start();
    znp_worker_task_start();
    znp_state_set(ZnpTransportState::Init);

    ESP_LOGI(TAG, "transport up on UART%d TX=%d RX=%d nRST=%d",
             znp_uart_port, PIN_TX, PIN_RX, PIN_NRESET);
}

void znp_hw_reset() {
    gpio_set_level(PIN_NRESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_NRESET, 1);
    ESP_LOGI(TAG, "CC2652 hardware reset asserted");
}
