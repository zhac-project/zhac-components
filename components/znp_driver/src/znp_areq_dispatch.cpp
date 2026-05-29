// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_areq_dispatch.cpp — subscriber registry for AREQ frames.
//
// AREQ is routed entirely separately from synchronous SRSP handling. The RX
// task classifies each frame and calls znp_areq_dispatch() directly — the
// worker task is never involved. This keeps async traffic from ever
// interacting with request/response bookkeeping.

#include "znp_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <utility>

static const char* TAG = "znp_areq";

static constexpr size_t MAX_HANDLERS = 24;

struct Entry {
    uint8_t        cmd0;
    uint8_t        cmd1;
    ZnpAreqHandler cb;
};

static Entry s_table[MAX_HANDLERS];
static size_t s_count = 0;

// F34 (FINDINGS.md): the table is mutated by znp_subscribe_areq (init +
// znp_confirm's lazy ensure_init, on various tasks) while znp_areq_dispatch
// iterates it on the RX task — a concurrent subscribe could otherwise be
// read mid-write (torn std::function). Created at C++ static-init (single
// threaded, before the scheduler dispatches tasks); `if (s_mtx)` guards
// degrade gracefully to the prior behavior should creation ever fail.
static SemaphoreHandle_t s_mtx = xSemaphoreCreateMutex();

void znp_subscribe_areq(uint8_t cmd0, uint8_t cmd1, ZnpAreqHandler cb) {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    // Replace an existing subscription for the same (cmd0, cmd1).
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].cmd0 == cmd0 && s_table[i].cmd1 == cmd1) {
            s_table[i].cb = std::move(cb);
            if (s_mtx) xSemaphoreGive(s_mtx);
            return;
        }
    }
    if (s_count >= MAX_HANDLERS) {
        if (s_mtx) xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "subscription table full — dropping handler for "
                      "cmd0=0x%02x cmd1=0x%02x", cmd0, cmd1);
        return;
    }
    s_table[s_count++] = { cmd0, cmd1, std::move(cb) };
    if (s_mtx) xSemaphoreGive(s_mtx);
}

void znp_areq_dispatch(const ZnpFrame& f) {
    // Snapshot the matching handler under the lock, then invoke it OUTSIDE
    // the lock — callbacks may block/allocate, so they must not run while
    // holding the table mutex. Subscriptions dedup by (cmd0,cmd1), so there
    // is at most one match.
    ZnpAreqHandler cb;
    bool found = false;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].cmd0 == f.cmd0 && s_table[i].cmd1 == f.cmd1) {
            cb = s_table[i].cb;   // copy under lock
            found = true;
            break;
        }
    }
    if (s_mtx) xSemaphoreGive(s_mtx);
    if (found && cb) cb(f);
}
