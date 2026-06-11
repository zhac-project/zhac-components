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
// within the flush window. That includes the NVS-commit window itself:
// a slot keeps its state+payload while its write is in flight
// (`flushing`) and goes CLEAN only after the commit lands, so overlay
// readers can't fall through to stale NVS mid-write.

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
static constexpr uint32_t FLUSH_WAIT_POLL_MS = 10;    // barrier poll period
// Barrier upper bound — ≫ worst-case NVS commit incl. page GC (tens of
// ms); generous so the barrier only expires on genuinely wedged flash.
static constexpr uint32_t FLUSH_WAIT_MAX_MS  = 5000;

enum DirtyState : uint8_t { CLEAN = 0, WRITE = 1, TOMBSTONE = 2 };

// Slot lifecycle (every transition happens under s_mtx):
//
//   CLEAN ──mark──▶ WRITE/TOMBSTONE ──flusher──▶ +flushing ──settle──▶ CLEAN
//
//   flushing + mark_dirty/mark_delete → op+payload replaced in place,
//       remarked=true; settle leaves the slot pending so the newer data
//       flushes next cycle (the flusher's older snapshot is discarded —
//       newer wins, no duplicate entry for the same rule_id possible).
//   flushing + NVS failure → settle leaves state+payload untouched and
//       retries next tick in place — no re-queue into a free slot, so a
//       stale snapshot can never race a newer mark into a second slot.
//
// `state` keeps carrying the op while the write is in flight, so the
// overlay readers (rule_store_load_overlay / rule_store_foreach_dirty)
// keep serving the pending edit until it is truly durable. Invariant:
// state == CLEAN implies !flushing — the free-list (find_free_locked)
// can never hand out a slot whose write is still in flight. Only the
// flusher that set `flushing` settles the slot; concurrent flushers
// back off, so ownership is unambiguous.

struct DirtySlot {
    uint16_t    rule_id;
    uint8_t     state;       // DirtyState — pending op; CLEAN = slot free
    bool        flushing;    // NVS op for this slot is in flight
    bool        remarked;    // newer mark landed while flushing
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

// Flush one slot outside s_mtx. Returns true when the pending op is
// durable (or nothing was pending / another flusher owns the slot);
// false when our NVS write failed and the slot stays pending for retry.
//
// The slot keeps its state+payload while the NVS op is in flight
// (`flushing` set), so overlay readers still see the edit and
// rule_store_flush_now() can wait on it; it goes CLEAN only after a
// successful, un-remarked commit.
static bool flush_slot(size_t idx) {
    DirtySlot snap;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_dirty[idx].state == CLEAN || s_dirty[idx].flushing) {
        xSemaphoreGive(s_mtx);
        return true;   // nothing pending, or in flight on another task
    }
    snap = s_dirty[idx];               // op+payload snapshot under lock
    s_dirty[idx].flushing = true;
    s_dirty[idx].remarked = false;
    xSemaphoreGive(s_mtx);

    bool ok = true;
    if (snap.state == WRITE) {
        ok = rule_store_save(&snap.slot);
        if (!ok)
            ESP_LOGE(TAG, "flush failed rule_id=0x%04x — retrying next tick",
                     snap.rule_id);
    } else if (snap.state == TOMBSTONE) {
        // rule_store_delete returns false if nothing was stored — that's
        // fine for a tombstone created before any commit landed. Don't
        // retry in that case.
        (void)rule_store_delete(snap.rule_id);
        ok = true;
    }

    // Settle. Only we (the flushing owner) may clean the slot; marks that
    // landed during the write replaced op+payload in place and set
    // `remarked` — they could not have moved to another slot because this
    // one stayed visible to find_slot_locked the whole time.
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_dirty[idx].flushing = false;
    if (s_dirty[idx].remarked) {
        // Newer mark_dirty/mark_delete during the NVS op: leave the slot
        // pending — newer wins, our snapshot is obsolete. (Replaces the
        // old failed-flush re-queue, which could insert a stale snapshot
        // next to a newer mark and persist the stale copy last.)
        s_dirty[idx].remarked = false;
    } else if (ok) {
        s_dirty[idx].state = CLEAN;    // durable — readers may use NVS now
    }
    // (!ok && !remarked): slot still holds the same op+payload → retried
    // next tick in place. No free-slot hunt, nothing to drop on a full
    // table — supersedes the F37 re-queue fallback.
    xSemaphoreGive(s_mtx);
    return ok;
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
    // Mark landed while this slot's previous op is mid-NVS-write: tell the
    // flusher to leave the slot pending so this newer payload flushes too.
    s_dirty[idx].remarked  = s_dirty[idx].flushing;
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
    // See rule_store_mark_dirty: keep the slot pending if a write for its
    // previous op is in flight — the tombstone must still be applied.
    s_dirty[idx].remarked  = s_dirty[idx].flushing;
    xSemaphoreGive(s_mtx);
}

// Durability barrier: wait (bounded) until no slot has an NVS op in
// flight. Returns false on timeout. Must not be called with s_mtx held.
static bool wait_no_flushing() {
    for (uint32_t waited = 0;; waited += FLUSH_WAIT_POLL_MS) {
        bool busy = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (s_dirty[i].flushing) { busy = true; break; }
        }
        xSemaphoreGive(s_mtx);
        if (!busy) return true;
        if (waited >= FLUSH_WAIT_MAX_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(FLUSH_WAIT_POLL_MS));
    }
}

void rule_store_flush_now() {
    if (!s_dirty || !s_mtx) return;
    // Two bounded passes. Pass 1 flushes everything pending; the barrier
    // then settles ops in flight on the tick task. Pass 2 catches slots
    // those ops left pending (re-marked mid-write) and retries failures
    // once. On return, every edit pending at call time is on flash —
    // except writes that failed twice, which flush_slot logs loudly.
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < DIRTY_CAP; i++) flush_slot(i);
        if (!wait_no_flushing())
            ESP_LOGE(TAG, "flush_now: in-flight NVS op did not settle within %lu ms",
                     (unsigned long)FLUSH_WAIT_MAX_MS);
        bool pending = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (s_dirty[i].state != CLEAN) { pending = true; break; }
        }
        xSemaphoreGive(s_mtx);
        if (!pending) return;
    }
    ESP_LOGE(TAG, "flush_now: pending entries remain (write failures or concurrent marks)");
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
            // flushing slots are owned by another flusher — skip.
            if (s_dirty[i].state == CLEAN || s_dirty[i].flushing) continue;
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
