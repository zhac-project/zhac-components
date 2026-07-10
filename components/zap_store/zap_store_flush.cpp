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
//     flush_now()/flush_device() are durability barriers: they only
//     return once in-flight NVS writes have settled, so "flushed" means
//     on-flash — not "the dirty table looked clean mid-write".

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
static constexpr uint32_t FLUSH_WAIT_POLL_MS = 10;    // barrier poll period
// Barrier upper bound — ≫ worst-case NVS commit incl. page GC (tens of
// ms); generous so the barrier only expires on genuinely wedged flash.
static constexpr uint32_t FLUSH_WAIT_MAX_MS  = 5000;

// Slot lifecycle (every transition happens under s_mtx):
//
//   FREE ──mark_dirty──▶ DIRTY ──flusher──▶ FLUSHING ──settle──▶ FREE
//
//   FLUSHING + mark_dirty       → remarked=true (state stays FLUSHING;
//                                 settle leaves the slot DIRTY so the
//                                 newer data flushes next cycle)
//   FLUSHING + NVS write failed → settle reverts to DIRTY (retry next
//                                 tick, in place — no re-queue)
//
// A FLUSHING slot is skipped by find_free_slot_locked, so it cannot be
// reused for a different IEEE while its NVS write is in flight, and the
// flush barriers (flush_now / flush_device) can wait on it. Only the
// flusher that set FLUSHING ever settles the slot; concurrent flushers
// see FLUSHING and back off, so ownership is unambiguous.
// CODEX ODR: `DirtySlot` (and the related state enum) is a private, file-local
// type — rule_store_flush.cpp defines a different `struct DirtySlot`. At
// external linkage that is a One Definition Rule violation. Anonymous namespace
// gives both internal linkage so the two never collide.
namespace {
enum SlotState : uint8_t { SLOT_FREE = 0, SLOT_DIRTY = 1, SLOT_FLUSHING = 2 };

struct DirtySlot {
    uint64_t ieee;
    uint32_t marked_ms;
    uint8_t  pri;
    uint8_t  state;      // SlotState
    bool     remarked;   // mark_dirty landed while FLUSHING — newer data pending
};
}  // namespace

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

// Find an existing entry for ieee (DIRTY or FLUSHING). Caller must hold s_mtx.
static int find_slot_locked(uint64_t ieee) {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].state != SLOT_FREE && s_dirty[i].ieee == ieee) return (int)i;
    }
    return -1;
}

static int find_free_slot_locked() {
    for (size_t i = 0; i < DIRTY_CAP; i++) {
        if (s_dirty[i].state == SLOT_FREE) return (int)i;
    }
    return -1;
}

// Outcome of one flush attempt. flush_device needs to distinguish
// "durable now" from "someone else's write is still in flight".
enum class FlushOutcome : uint8_t {
    Settled,  // persisted, dropped, or slot vacated — nothing of ours in flight
    Retry,    // our NVS write failed; slot reverted to DIRTY for next tick
    Busy,     // another flusher owns the slot (FLUSHING) — write in flight
};

// Flush one dirty slot. Caller must NOT hold s_mtx (NVS write can block).
// `expect_ieee` (optional) makes the attempt a no-op if the slot no longer
// holds that device — closes flush_device's find→flush window where the
// slot could settle and be reused for a different IEEE.
//
// The slot stays FLUSHING (visible to barriers, skipped by the free-list)
// for the whole NVS write and is settled only afterwards — and only to
// FREE if no mark_dirty re-marked it during the write. On failure it
// reverts to DIRTY in place: no free-slot hunt, nothing to drop on a
// full table (supersedes the old F37 re-queue fallback).
static FlushOutcome flush_slot(size_t idx, const uint64_t* expect_ieee = nullptr) {
    DirtySlot snap;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_dirty[idx].state == SLOT_FLUSHING) {
        xSemaphoreGive(s_mtx);
        return FlushOutcome::Busy;      // another task is mid-write here
    }
    if (s_dirty[idx].state == SLOT_FREE ||
        (expect_ieee && s_dirty[idx].ieee != *expect_ieee)) {
        xSemaphoreGive(s_mtx);
        return FlushOutcome::Settled;   // nothing (of ours) pending here
    }
    snap = s_dirty[idx];
    s_dirty[idx].state    = SLOT_FLUSHING;  // own it for the I/O window
    s_dirty[idx].remarked = false;
    xSemaphoreGive(s_mtx);

    bool persisted = false;
    bool dropped   = false;
    if (!s_snapshot) {
        ESP_LOGW(TAG, "no snapshot_cb — dropping dirty ieee=0x%016llx",
                 (unsigned long long)snap.ieee);
        dropped = true;
    } else {
        ZapDevice dev{};
        if (!s_snapshot(snap.ieee, &dev)) {
            ESP_LOGD(TAG, "snapshot missing for ieee=0x%016llx — skip",
                     (unsigned long long)snap.ieee);
            dropped = true;   // device left the pool; nothing to persist
        } else if (!(persisted = zap_store_save_device(&dev))) {
            ESP_LOGE(TAG, "save failed ieee=0x%016llx — retrying next tick",
                     (unsigned long long)snap.ieee);
        }
    }

    // Settle. Only we (the FLUSHING owner) may free the slot; mark_dirty
    // can't have re-issued it to another IEEE meanwhile (not FREE), so
    // `remarked` unambiguously means "newer data arrived during the write".
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_dirty[idx].remarked) {
        // Keep the entry DIRTY so the newer state flushes next cycle
        // (marked_ms is pre-write → due immediately).
        s_dirty[idx].state    = SLOT_DIRTY;
        s_dirty[idx].remarked = false;
    } else if (persisted || dropped) {
        s_dirty[idx].state = SLOT_FREE;
    } else {
        s_dirty[idx].state = SLOT_DIRTY;    // failed write → retry next tick
    }
    xSemaphoreGive(s_mtx);
    return (persisted || dropped) ? FlushOutcome::Settled : FlushOutcome::Retry;
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
        // Stack-snapshot the device first: callers from the interview
        // path pass `dev` into the live pool, and the flush-mutex release
        // above can race with a `pool_remove` (swap-with-last) that
        // relocates the entry at `dev` to a different device — passing
        // the raw pointer to save_device would then write the wrong
        // record under our IEEE.
        ZapDevice snap = *dev;
        ESP_LOGW(TAG, "dirty table full — immediate save ieee=0x%016llx",
                 (unsigned long long)snap.ieee_addr);
        zap_store_save_device(&snap);
        return;
    }
    // If entry exists, upgrade priority (HIGH wins) but keep original
    // marked_ms so older HIGH writes don't get delayed by renewed LOW.
    if (s_dirty[slot].state == SLOT_FREE) {
        s_dirty[slot].ieee      = dev->ieee_addr;
        s_dirty[slot].marked_ms = now_ms();
        s_dirty[slot].pri       = (uint8_t)pri;
        s_dirty[slot].state     = SLOT_DIRTY;
        s_dirty[slot].remarked  = false;
    } else {
        if ((uint8_t)pri > s_dirty[slot].pri) s_dirty[slot].pri = (uint8_t)pri;
        if (s_dirty[slot].state == SLOT_FLUSHING) {
            // A write for this device is in flight with pre-mark data.
            // Flag it so the flusher leaves the slot DIRTY instead of
            // freeing it — this newer state must flush next cycle.
            s_dirty[slot].remarked = true;
        }
    }
    xSemaphoreGive(s_mtx);
}

// Durability barrier: wait (bounded) until no slot is mid-NVS-write.
// Returns false on timeout. Must not be called with s_mtx held.
static bool wait_no_flushing() {
    for (uint32_t waited = 0;; waited += FLUSH_WAIT_POLL_MS) {
        bool busy = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (s_dirty[i].state == SLOT_FLUSHING) { busy = true; break; }
        }
        xSemaphoreGive(s_mtx);
        if (!busy) return true;
        if (waited >= FLUSH_WAIT_MAX_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(FLUSH_WAIT_POLL_MS));
    }
}

bool zap_store_flush_device(uint64_t ieee) {
    if (!s_mtx) return true;
    uint32_t waited_ms = 0;
    for (;;) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        int slot = find_slot_locked(ieee);
        uint8_t st = (slot >= 0) ? s_dirty[slot].state : (uint8_t)SLOT_FREE;
        xSemaphoreGive(s_mtx);
        if (slot < 0) return true;   // not dirty — nothing pending for this device
        if (st == SLOT_DIRTY) {
            // flush_slot revalidates the IEEE under its own lock — the slot
            // may settle and be reused for another device between our peek
            // above and its take.
            FlushOutcome o = flush_slot((size_t)slot, &ieee);
            if (o == FlushOutcome::Settled) return true;
            if (o == FlushOutcome::Retry)   return false;
            // Busy: lost the race to another flusher — wait below.
        }
        // SLOT_FLUSHING: a write for this device is in flight on another
        // task. Wait (bounded) for it to settle, then re-evaluate — if the
        // slot was re-marked during that write we flush the newer data
        // ourselves on the next pass.
        if (waited_ms >= FLUSH_WAIT_MAX_MS) {
            ESP_LOGW(TAG, "flush_device: write still in flight after %lu ms ieee=0x%016llx",
                     (unsigned long)waited_ms, (unsigned long long)ieee);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(FLUSH_WAIT_POLL_MS));
        waited_ms += FLUSH_WAIT_POLL_MS;
    }
}

void zap_store_flush_now() {
    if (!s_mtx) return;
    // Two bounded passes. Pass 1 flushes everything DIRTY; the barrier then
    // settles writes owned by other tasks (tick task mid-write when we were
    // called). Pass 2 catches slots those writes left DIRTY (re-marked
    // during flight) and gives failed writes one retry. On return, every
    // entry that was dirty at call time is on flash — except writes that
    // failed twice, which are logged loudly by flush_slot.
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < DIRTY_CAP; i++) flush_slot(i);
        if (!wait_no_flushing())
            ESP_LOGE(TAG, "flush_now: in-flight write did not settle within %lu ms",
                     (unsigned long)FLUSH_WAIT_MAX_MS);
        bool pending = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (size_t i = 0; i < DIRTY_CAP; i++) {
            if (s_dirty[i].state != SLOT_FREE) { pending = true; break; }
        }
        xSemaphoreGive(s_mtx);
        if (!pending) return;
    }
    ESP_LOGE(TAG, "flush_now: dirty entries remain (write failures or concurrent marks)");
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
            // FLUSHING slots are owned by another flusher — skip.
            if (s_dirty[i].state != SLOT_DIRTY) continue;
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
    if (xTaskCreate(flush_task, "zap_flush", zhac::stack::kZapFlush,
                    nullptr, 3, nullptr) != pdPASS) {
        // P1-T8: with the flag left true, dirty marks would defer into a
        // table no task ever drains (silent persistence loss). Roll back
        // so marks fall through to the immediate-save path.
        s_task_started = false;
        ESP_LOGE(TAG, "flush task create failed — writeback disabled, "
                      "saves go direct to NVS");
    }
}
