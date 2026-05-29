// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/rule_store/rule_store_flush.cpp
//
// PSRAM-backed writeback cache for RuleSlot NVS commits. Mirrors the
// zap_store_flush pattern. User-triggered edits (REST / WebUI) are
// queued in RAM and flushed by a 1 s tick task, either on age (≥5 s) or
// explicit `rule_store_flush_now()` (shutdown / OTA).
//
// Read consistency: `rule_store_load` + `rule_store_load_all` consult
// the dirty table first — callers never observe stale NVS state even
// within the flush window.

#include "rule_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include "task_stacks.h"

static const char* TAG = "rule_flush";

static constexpr uint32_t FLUSH_TICK_MS   = 1000;
static constexpr uint32_t MAX_AGE_MS      = 5 * 1000;
static constexpr size_t   DIRTY_CAP       = 64;  // concurrent dirty rules

enum DirtyState : uint8_t { CLEAN = 0, WRITE = 1, TOMBSTONE = 2 };

struct DirtySlot {
    uint16_t    rule_id;
    uint8_t     state;       // DirtyState
    uint32_t    marked_ms;
    RuleSlot    slot;        // full payload for WRITE; zeroed for TOMBSTONE
};

static DirtySlot*        s_dirty = nullptr;  // allocated in PSRAM
static SemaphoreHandle_t s_mtx   = nullptr;
static bool              s_task_started = false;

static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int find_slot_locked(uint16_t rule_id) {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].state != CLEAN && s_dirty[i].rule_id == rule_id) return (int)i;
    }
    return -1;
}

static int find_free_locked() {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].state == CLEAN) return (int)i;
    }
    return -1;
}

// Flush one slot outside s_mtx. Returns true when slot is handled.
static bool flush_slot(size_t idx) {
    DirtySlot snap;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_dirty[idx].state == CLEAN) {
        xSemaphoreGive(s_mtx);
        return true;
    }
    snap = s_dirty[idx];
    s_dirty[idx].state = CLEAN;
    xSemaphoreGive(s_mtx);

    bool ok = true;
    if (snap.state == WRITE) {
        ok = rule_store_save(&snap.slot);
    } else if (snap.state == TOMBSTONE) {
        // rule_store_delete returns false if nothing was stored — that's
        // fine for a tombstone created before any commit landed. Don't
        // re-queue in that case.
        (void)rule_store_delete(snap.rule_id);
        ok = true;
    }
    if (!ok) {
        // Only WRITE reaches here — TOMBSTONE always sets ok=true above.
        ESP_LOGE(TAG, "flush failed rule_id=0x%04x state=%u — re-queueing",
                 snap.rule_id, snap.state);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        int slot = find_free_locked();
        if (slot >= 0) s_dirty[slot] = snap;
        xSemaphoreGive(s_mtx);
        if (slot < 0) {
            // F37 (FINDINGS.md): dirty table full — we can't defer the retry,
            // so persist synchronously here instead of silently dropping the
            // edit. May fail again (same NVS error), but then it's logged loud,
            // not lost in silence.
            ESP_LOGW(TAG, "dirty table full on re-queue — synchronous save rule_id=0x%04x",
                     snap.rule_id);
            if (!rule_store_save(&snap.slot))
                ESP_LOGE(TAG, "synchronous save ALSO failed rule_id=0x%04x — edit lost",
                         snap.rule_id);
            return true;   // slot consumed; nothing left re-queued
        }
        return false;
    }
    return true;
}

void rule_store_mark_dirty(const RuleSlot* slot) {
    if (!slot) return;
    if (!s_mtx || !s_task_started || !s_dirty) {
        rule_store_save(slot);
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = find_slot_locked(slot->rule_id);
    if (idx < 0) idx = find_free_locked();
    if (idx < 0) {
        xSemaphoreGive(s_mtx);
        ESP_LOGW(TAG, "dirty table full — immediate save rule_id=0x%04x",
                 slot->rule_id);
        rule_store_save(slot);
        return;
    }
    s_dirty[idx].rule_id   = slot->rule_id;
    s_dirty[idx].state     = WRITE;
    s_dirty[idx].marked_ms = now_ms();
    s_dirty[idx].slot      = *slot;
    xSemaphoreGive(s_mtx);
}

void rule_store_mark_delete(uint16_t rule_id) {
    if (!s_mtx || !s_task_started || !s_dirty) {
        rule_store_delete(rule_id);
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = find_slot_locked(rule_id);
    if (idx < 0) idx = find_free_locked();
    if (idx < 0) {
        xSemaphoreGive(s_mtx);
        ESP_LOGW(TAG, "dirty table full — immediate delete rule_id=0x%04x",
                 rule_id);
        rule_store_delete(rule_id);
        return;
    }
    s_dirty[idx].rule_id   = rule_id;
    s_dirty[idx].state     = TOMBSTONE;
    s_dirty[idx].marked_ms = now_ms();
    memset(&s_dirty[idx].slot, 0, sizeof(RuleSlot));
    xSemaphoreGive(s_mtx);
}

void rule_store_flush_now() {
    if (!s_dirty || !s_mtx) return;
    for (size_t i = 0; i < DIRTY_CAP; i++) flush_slot(i);
}

// Overlay APIs — consult dirty table before falling through to NVS so
// readers see the latest edit regardless of flush timing.
//
// F-02 fix: distinguish "found-as-tombstone" from "not in overlay" via
// out_tombstoned (NULL allowed). Both still return false, but the
// caller (rule_store_load) checks the flag to skip the NVS fallthrough
// — otherwise a deleted rule reappears until flush_task fires (~5 s),
// and permanently if power is cut before flush.
extern "C" bool rule_store_load_overlay(uint16_t rule_id, RuleSlot* out,
                                         bool* out_tombstoned) {
    if (out_tombstoned) *out_tombstoned = false;
    if (!s_dirty || !s_mtx) return false;
    bool hit = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = find_slot_locked(rule_id);
    if (idx >= 0) {
        if (s_dirty[idx].state == TOMBSTONE) {
            if (out_tombstoned) *out_tombstoned = true;
            xSemaphoreGive(s_mtx);
            return false;
        }
        if (s_dirty[idx].state == WRITE) {
            *out = s_dirty[idx].slot;
            hit = true;
        }
    }
    xSemaphoreGive(s_mtx);
    return hit;
}

// List iteration: returns array of (rule_id, tombstoned?, slot) entries
// so rule_store_load_all can merge them with NVS iteration results.
extern "C" void rule_store_foreach_dirty(
    void (*cb)(uint16_t rule_id, bool tombstoned, const RuleSlot* slot, void* ctx),
    void* ctx)
{
    if (!s_dirty || !s_mtx || !cb) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].state == CLEAN) continue;
        cb(s_dirty[i].rule_id,
           s_dirty[i].state == TOMBSTONE,
           s_dirty[i].state == WRITE ? &s_dirty[i].slot : nullptr,
           ctx);
    }
    xSemaphoreGive(s_mtx);
}

static void flush_task(void*) {
    ESP_LOGI(TAG, "rule_store flush task started (cap=%u, max_age=%lums)",
             (unsigned)DIRTY_CAP, (unsigned long)MAX_AGE_MS);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_TICK_MS));
        const uint32_t now = now_ms();
        size_t due[DIRTY_CAP];
        size_t due_n = 0;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (s_dirty[i].state == CLEAN) continue;
            if (now - s_dirty[i].marked_ms >= MAX_AGE_MS) {
                due[due_n++] = i;
            }
        }
        xSemaphoreGive(s_mtx);
        for (size_t k = 0; k < due_n; k++) flush_slot(due[k]);
    }
}

void rule_store_flush_init() {
    if (s_task_started) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    configASSERT(s_mtx);
    if (!s_dirty) {
        s_dirty = static_cast<DirtySlot*>(
            heap_caps_calloc(DIRTY_CAP, sizeof(DirtySlot),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_dirty) {
            ESP_LOGE(TAG, "PSRAM alloc failed — writeback disabled, "
                          "edits go direct to NVS");
            s_task_started = false;
            return;
        }
    }
    s_task_started = true;
    // 3072 was overflowing under burst: due[DIRTY_CAP]=256B + NVS commit
    // path (~2 KB) + printf in log line (~1.5 KB) easily exceeds. 6144
    // gives ~2x headroom and matches other NVS-touching tasks.
    xTaskCreate(flush_task, "rule_flush", zhac::stack::kRuleFlush, nullptr, 3, nullptr);
}
