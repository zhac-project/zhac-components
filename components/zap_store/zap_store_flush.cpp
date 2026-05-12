// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zap_store/zap_store_flush.cpp
//
// Writeback cache for ZapDevice persistence. Holds a small dirty-table
// keyed by IEEE; a 1 s tick task flushes entries whose age exceeds the
// per-priority threshold. User-visible changes (rename, interview end)
// get HIGH priority (≤5 s latency). Runtime bookkeeping (identity,
// configure) gets LOW (≤300 s), which cuts flash wear on busy fleets.
//
// Call-site contract:
//   - zap_store_set_snapshot_cb(cb)   — install during init (zigbee_mgr)
//   - zap_store_mark_dirty(dev, pri)  — replaces most zap_store_save_device
//   - zap_store_flush_now()           — shutdown / OTA handoff
//
// Power-loss contract:
//   - LOW-priority data can lose up to 300 s on a hard crash.
//   - HIGH-priority data can lose up to 5 s.
//   - Graceful reboots + OTA register shutdown handlers to flush first.

#include "zap_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include "task_stacks.h"

static const char* TAG = "zap_flush";

static constexpr uint32_t FLUSH_TICK_MS   = 1000;
static constexpr uint32_t HIGH_MAX_AGE_MS = 5 * 1000;
static constexpr uint32_t LOW_MAX_AGE_MS  = 300 * 1000;
static constexpr size_t   DIRTY_CAP       = 64;  // concurrent dirty entries

struct DirtySlot {
    uint64_t ieee;
    uint32_t marked_ms;
    uint8_t  pri;
    bool     in_use;
};

static DirtySlot            s_dirty[DIRTY_CAP] = {};
static SemaphoreHandle_t    s_mtx = nullptr;
static ZapStoreSnapshotCb   s_snapshot = nullptr;
static bool                 s_task_started = false;

static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void zap_store_set_snapshot_cb(ZapStoreSnapshotCb cb) {
    s_snapshot = cb;
}

// Find an existing entry for ieee. Caller must hold s_mtx.
static int find_slot_locked(uint64_t ieee) {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].in_use && s_dirty[i].ieee == ieee) return (int)i;
    }
    return -1;
}

static int find_free_slot_locked() {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (!s_dirty[i].in_use) return (int)i;
    }
    return -1;
}

// Flush one dirty slot. Caller must NOT hold s_mtx (NVS write can block).
// Returns true if the slot was consumed (dropped or persisted).
static bool flush_slot(size_t idx) {
    DirtySlot snap;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_dirty[idx].in_use) { xSemaphoreGive(s_mtx); return true; }
    snap = s_dirty[idx];
    // Mark entry free now; if snapshot or save fails we've still cleared
    // the dirty state. Future mark_dirty calls will re-add it.
    s_dirty[idx].in_use = false;
    xSemaphoreGive(s_mtx);

    if (!s_snapshot) {
        ESP_LOGW(TAG, "no snapshot_cb — dropping dirty ieee=0x%016llx",
                 (unsigned long long)snap.ieee);
        return true;
    }
    ZapDevice dev{};
    if (!s_snapshot(snap.ieee, &dev)) {
        ESP_LOGD(TAG, "snapshot missing for ieee=0x%016llx — skip",
                 (unsigned long long)snap.ieee);
        return true;
    }
    if (!zap_store_save_device(&dev)) {
        ESP_LOGE(TAG, "save failed ieee=0x%016llx — re-queueing",
                 (unsigned long long)snap.ieee);
        // Re-queue under original priority so we retry next tick.
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        int slot = find_free_slot_locked();
        if (slot >= 0) {
            s_dirty[slot] = snap;
            s_dirty[slot].in_use = true;
        }
        xSemaphoreGive(s_mtx);
        return false;
    }
    return true;
}

void zap_store_mark_dirty(const ZapDevice* dev, ZapPersistPriority pri) {
    if (!dev) return;
    // No writeback task yet (init order): fall back to immediate write.
    if (!s_mtx || !s_task_started) {
        zap_store_save_device(dev);
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int slot = find_slot_locked(dev->ieee_addr);
    if (slot < 0) slot = find_free_slot_locked();
    if (slot < 0) {
        xSemaphoreGive(s_mtx);
        // Dirty-table full: force immediate persist as fallback to avoid
        // losing the write. Rare — 64 concurrent dirty devices is a lot.
        ESP_LOGW(TAG, "dirty table full — immediate save ieee=0x%016llx",
                 (unsigned long long)dev->ieee_addr);
        zap_store_save_device(dev);
        return;
    }
    // If entry exists, upgrade priority (HIGH wins) but keep original
    // marked_ms so older HIGH writes don't get delayed by renewed LOW.
    bool existed = s_dirty[slot].in_use;
    if (!existed) {
        s_dirty[slot].ieee      = dev->ieee_addr;
        s_dirty[slot].marked_ms = now_ms();
        s_dirty[slot].pri       = (uint8_t)pri;
        s_dirty[slot].in_use    = true;
    } else {
        if ((uint8_t)pri > s_dirty[slot].pri) s_dirty[slot].pri = (uint8_t)pri;
    }
    xSemaphoreGive(s_mtx);
}

bool zap_store_flush_device(uint64_t ieee) {
    if (!s_mtx) return true;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int slot = find_slot_locked(ieee);
    xSemaphoreGive(s_mtx);
    if (slot < 0) return true;   // not dirty
    return flush_slot((size_t)slot);
}

void zap_store_flush_now() {
    if (!s_mtx) return;
    for (size_t i = 0; i < DIRTY_CAP; i++) flush_slot(i);
}

static void flush_task(void*) {
    ESP_LOGI(TAG, "flush task started tick=%lums high=%lums low=%lums",
             (unsigned long)FLUSH_TICK_MS,
             (unsigned long)HIGH_MAX_AGE_MS,
             (unsigned long)LOW_MAX_AGE_MS);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_TICK_MS));
        const uint32_t now = now_ms();
        // Snapshot indices to flush (under lock), then flush outside lock.
        size_t due[DIRTY_CAP];
        size_t due_n = 0;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (!s_dirty[i].in_use) continue;
            const uint32_t age = now - s_dirty[i].marked_ms;
            const uint32_t cap = (s_dirty[i].pri == ZAP_PERSIST_HIGH)
                                   ? HIGH_MAX_AGE_MS : LOW_MAX_AGE_MS;
            if (age >= cap) due[due_n++] = i;
        }
        xSemaphoreGive(s_mtx);
        for (size_t k = 0; k < due_n; k++) flush_slot(due[k]);
    }
}

void zap_store_flush_init() {
    if (s_task_started) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    configASSERT(s_mtx);
    s_task_started = true;
    xTaskCreate(flush_task, "zap_flush", zhac::stack::kZapFlush, nullptr, 3, nullptr);
}
