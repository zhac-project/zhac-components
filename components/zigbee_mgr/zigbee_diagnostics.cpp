// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zigbee_diagnostics.cpp
//
// 32-slot unhandled-frame ring. When the ZCL dispatch pipeline produces
// zero attrs for a frame (all decoders miss), we record (cluster, attr
// or cmd, cluster_specific, ieee, ts, count). Entries are keyed by the
// triple (cluster, attr_or_cmd, cluster_specific) so repeat hits bump a
// counter rather than flooding the ring.
//
// Not CRITICAL but feeds a future Diagnostics UI tab + drives
// prioritization for pipeline converter-body extraction work.

#include "zigbee_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdlib>

static const char* TAG = "zb_diag";

namespace {

constexpr size_t SLOTS = 32;

struct Slot : ZbUnhandledFrame {
    bool in_use;
};

Slot*             s_ring = nullptr;
SemaphoreHandle_t s_mtx  = nullptr;

uint32_t now_s() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

int find_slot_locked(uint16_t cluster, uint16_t ac, bool cs) {
    for (size_t i = 0; i < SLOTS; i++) {
        if (!s_ring[i].in_use) continue;
        if (s_ring[i].cluster_id == cluster &&
            s_ring[i].attr_or_cmd_id == ac &&
            static_cast<bool>(s_ring[i].cluster_specific) == cs)
            return static_cast<int>(i);
    }
    return -1;
}

int find_free_or_lru_locked() {
    int free_idx = -1;
    int lru_idx = 0;
    uint32_t lru_ts = 0xFFFFFFFFu;
    for (size_t i = 0; i < SLOTS; i++) {
        if (!s_ring[i].in_use) {
            if (free_idx < 0) free_idx = static_cast<int>(i);
        } else if (s_ring[i].last_seen_s < lru_ts) {
            lru_ts = s_ring[i].last_seen_s;
            lru_idx = static_cast<int>(i);
        }
    }
    return free_idx >= 0 ? free_idx : lru_idx;
}

}  // namespace

void zb_diag_init() {
    if (s_ring) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    configASSERT(s_mtx);
    s_ring = static_cast<Slot*>(
        heap_caps_calloc(SLOTS, sizeof(Slot),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_ring) {
        ESP_LOGE(TAG, "PSRAM alloc failed — diagnostics disabled");
        return;
    }
    ESP_LOGI(TAG, "unhandled-frame ring ready (%u slots)", (unsigned)SLOTS);
}

void zb_diag_record_unhandled(uint16_t cluster_id, uint16_t attr_or_cmd_id,
                               bool cluster_specific, uint64_t ieee) {
    if (!s_ring || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = find_slot_locked(cluster_id, attr_or_cmd_id, cluster_specific);
    if (idx < 0) {
        idx = find_free_or_lru_locked();
        s_ring[idx].in_use           = true;
        s_ring[idx].cluster_id       = cluster_id;
        s_ring[idx].attr_or_cmd_id   = attr_or_cmd_id;
        s_ring[idx].cluster_specific = cluster_specific ? 1 : 0;
        s_ring[idx].count            = 0;
    }
    s_ring[idx].count++;
    s_ring[idx].last_seen_s = now_s();
    s_ring[idx].last_ieee   = ieee;
    xSemaphoreGive(s_mtx);
}

static int cmp_desc(const void* a, const void* b) {
    const auto* ea = static_cast<const ZbUnhandledFrame*>(a);
    const auto* eb = static_cast<const ZbUnhandledFrame*>(b);
    if (eb->last_seen_s > ea->last_seen_s) return 1;
    if (eb->last_seen_s < ea->last_seen_s) return -1;
    return 0;
}

uint16_t zb_diag_snapshot(ZbUnhandledFrame* out, uint16_t max) {
    if (!s_ring || !s_mtx || !out || max == 0) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint16_t n = 0;
    for (size_t i = 0; i < SLOTS && n < max; i++) {
        if (s_ring[i].in_use) {
            out[n++] = s_ring[i];
        }
    }
    xSemaphoreGive(s_mtx);
    if (n > 1) qsort(out, n, sizeof(ZbUnhandledFrame), cmp_desc);
    return n;
}

void zb_diag_reset() {
    if (!s_ring || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(s_ring, 0, SLOTS * sizeof(Slot));
    xSemaphoreGive(s_mtx);
}
