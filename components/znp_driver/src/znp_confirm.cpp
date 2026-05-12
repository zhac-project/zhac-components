// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_confirm.cpp — 16-slot ring correlating AF_DATA_REQUEST trans_ids
// with their asynchronous AF_DATA_CONFIRM AREQs (cmd0=0x44 cmd1=0x80).

#include "znp_confirm.h"
#include "znp_internal.h"
#include "znp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <atomic>

namespace {

constexpr const char* TAG = "znp_confirm";
constexpr size_t      kSlotCount = 16;

struct Slot {
    uint8_t              trans_id;
    uint8_t              status;
    bool                 in_use;
    bool                 signaled;
    StaticSemaphore_t    sem_buf;
    SemaphoreHandle_t    sem;
};

Slot                   s_slots[kSlotCount];
SemaphoreHandle_t      s_mutex;
StaticSemaphore_t      s_mutex_buf;
std::atomic<bool>      s_init_started{false};
std::atomic<bool>      s_init_done{false};

void on_af_data_confirm(const ZnpFrame& f) {
    // MT AF_DATA_CONFIRM payload: status u8, endpoint u8, trans_id u8.
    if (f.len < 3) return;
    const uint8_t status   = f.data[0];
    const uint8_t trans_id = f.data[2];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < kSlotCount; i++) {
        if (s_slots[i].in_use && !s_slots[i].signaled
             && s_slots[i].trans_id == trans_id) {
            s_slots[i].status   = status;
            s_slots[i].signaled = true;
            xSemaphoreGive(s_slots[i].sem);
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    xSemaphoreGive(s_mutex);
    // No waiter — normal when the caller opted out, so stay quiet.
}

void ensure_init() {
    // Two-phase so a racing caller blocks on the completion flag
    // rather than a half-initialised mutex.
    bool expected = false;
    if (s_init_started.compare_exchange_strong(expected, true)) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
        for (size_t i = 0; i < kSlotCount; i++) {
            s_slots[i].in_use   = false;
            s_slots[i].signaled = false;
            s_slots[i].sem      = xSemaphoreCreateBinaryStatic(
                                    &s_slots[i].sem_buf);
        }
        znp_subscribe_areq(0x44, 0x80, on_af_data_confirm);
        s_init_done.store(true);
    } else {
        while (!s_init_done.load()) {
            vTaskDelay(1);
        }
    }
}

}  // namespace

int znp_confirm_reserve(uint8_t trans_id) {
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int handle = -1;
    for (size_t i = 0; i < kSlotCount; i++) {
        if (!s_slots[i].in_use) {
            // Drain any stale give from a prior use.
            while (xSemaphoreTake(s_slots[i].sem, 0) == pdTRUE) {}
            s_slots[i].in_use   = true;
            s_slots[i].trans_id = trans_id;
            s_slots[i].signaled = false;
            s_slots[i].status   = 0xFF;
            handle = static_cast<int>(i);
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (handle < 0) {
        ESP_LOGW(TAG, "ring full — dropping confirm wait for trans_id=0x%02x",
                 trans_id);
    }
    return handle;
}

int znp_confirm_wait(int slot, uint32_t timeout_ms) {
    if (slot < 0 || slot >= static_cast<int>(kSlotCount)) return -1;
    Slot& s = s_slots[slot];
    const BaseType_t got = xSemaphoreTake(s.sem, pdMS_TO_TICKS(timeout_ms));
    int result;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (got == pdTRUE && s.signaled) {
        result = static_cast<int>(s.status);
    } else {
        result = -1;
    }
    s.in_use   = false;
    s.signaled = false;
    xSemaphoreGive(s_mutex);
    return result;
}

void znp_confirm_release(int slot) {
    if (slot < 0 || slot >= static_cast<int>(kSlotCount)) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_slots[slot].in_use   = false;
    s_slots[slot].signaled = false;
    xSemaphoreGive(s_mutex);
}
