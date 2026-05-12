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

void znp_subscribe_areq(uint8_t cmd0, uint8_t cmd1, ZnpAreqHandler cb) {
    // Replace an existing subscription for the same (cmd0, cmd1).
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].cmd0 == cmd0 && s_table[i].cmd1 == cmd1) {
            s_table[i].cb = std::move(cb);
            return;
        }
    }
    if (s_count >= MAX_HANDLERS) {
        ESP_LOGE(TAG, "subscription table full — dropping handler for "
                      "cmd0=0x%02x cmd1=0x%02x", cmd0, cmd1);
        return;
    }
    s_table[s_count++] = { cmd0, cmd1, std::move(cb) };
}

void znp_areq_dispatch(const ZnpFrame& f) {
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].cmd0 == f.cmd0 && s_table[i].cmd1 == f.cmd1) {
            s_table[i].cb(f);
        }
    }
}
