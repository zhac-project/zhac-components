// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// zigbee_configure_queue.cpp — deferred configure worker with exponential
// backoff and dedup via persisted ConfigureState.
//
// Flow:
//   enqueue(ieee) → FreeRTOS queue → worker pops → pool snapshot →
//   if ConfigureState::DONE, skip; else if support_state != MATCHED, skip;
//   else run zcl_converter_configure; update state + attempts; persist;
//   on failure schedule a one-shot timer to re-enqueue after backoff.
//
// Backoff schedule (seconds): 1, 5, 30, 120, 600. After MAX_ATTEMPTS
// failures the device stays in ConfigureState::FAILED until it rejoins
// (which resets configure_attempts in zigbee_interview.cpp).

#include "zigbee_configure_queue.h"
#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "zap_store.h"
#include "zap_common.h"
#include "zhc_adapter.h"
#include "esp_log.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <cstdlib>
#include "task_stacks.h"

static const char* TAG = "zigbee_configure";

// Sized to ZAP_MAX_DEVICES so a mass-rejoin storm (coordinator restart with
// 20+ paired routers) never drops a configure. Each slot is 8 B; 200 × 8 =
// 1.6 KB, negligible on PSRAM. Previously depth=16 silently dropped
// always-on routers (Tuya, Hue) that never rejoin again — they stayed in
// ConfigureState::PENDING with no bindings, no reports, and a UI that
// claimed "supported" while the device was inert.
static constexpr uint16_t CONFIGURE_QUEUE_DEPTH = ZAP_MAX_DEVICES;
static constexpr uint8_t  MAX_ATTEMPTS          = 5;
static constexpr uint32_t BACKOFF_SECS[MAX_ATTEMPTS] = { 1, 5, 30, 120, 600 };

static QueueHandle_t s_q = nullptr;

// F49 (FINDINGS.md): reusable retry-timer pool. Previously every retry did a
// heap `new RetrySlot` + `xTimerCreate`/`xTimerDelete` (per-retry alloc + timer
// churn on flaky devices). Now a small pool of timers is created lazily and
// reused; the slot index (which fits in the timer ID) replaces the heap slot,
// so there is no per-retry allocation. The busy map is guarded by a short
// critical section (no blocking inside); all xTimer* calls run outside it.
static constexpr uint8_t RETRY_POOL = 16;
static TimerHandle_t s_retry_tmr[RETRY_POOL]  = {};
static uint64_t      s_retry_ieee[RETRY_POOL] = {};
static bool          s_retry_busy[RETRY_POOL] = {};
static portMUX_TYPE  s_retry_mux = portMUX_INITIALIZER_UNLOCKED;

static void retry_timer_cb(TimerHandle_t t) {
    const int slot = static_cast<int>(reinterpret_cast<intptr_t>(pvTimerGetTimerID(t)));
    uint64_t ieee = 0;
    bool ok = false;
    portENTER_CRITICAL(&s_retry_mux);
    if (slot >= 0 && slot < RETRY_POOL && s_retry_busy[slot]) {
        ieee = s_retry_ieee[slot];          // copy before releasing the slot
        s_retry_busy[slot] = false;         // one-shot already stopped — back to pool
        ok = true;
    }
    portEXIT_CRITICAL(&s_retry_mux);
    if (ok && s_q) (void)xQueueSend(s_q, &ieee, 0);
    // Timer is retained for reuse — no xTimerDelete.
}

static void schedule_retry(uint64_t ieee, uint8_t attempts) {
    uint8_t idx = (attempts == 0) ? 0 :
                  (attempts - 1 < MAX_ATTEMPTS ? attempts - 1 : MAX_ATTEMPTS - 1);
    uint32_t secs = BACKOFF_SECS[idx];

    int slot = -1;
    portENTER_CRITICAL(&s_retry_mux);
    for (int i = 0; i < RETRY_POOL; i++) {
        if (!s_retry_busy[i]) { s_retry_busy[i] = true; s_retry_ieee[i] = ieee; slot = i; break; }
    }
    portEXIT_CRITICAL(&s_retry_mux);

    if (slot < 0) {
        // Pool exhausted (>RETRY_POOL devices retrying at once) — re-queue now
        // rather than dropping; the worker re-attempts immediately.
        ESP_LOGW(TAG, "retry pool full — re-queue ieee=0x%016llx now",
                 (unsigned long long)ieee);
        if (s_q) (void)xQueueSend(s_q, &ieee, 0);
        return;
    }

    const TickType_t period = pdMS_TO_TICKS(secs * 1000UL);
    bool started;
    if (!s_retry_tmr[slot]) {
        s_retry_tmr[slot] = xTimerCreate(
            "cfg_retry", period, pdFALSE,
            reinterpret_cast<void*>(static_cast<intptr_t>(slot)), retry_timer_cb);
        started = (s_retry_tmr[slot] != nullptr) &&
                  (xTimerStart(s_retry_tmr[slot], 0) == pdPASS);
    } else {
        // Reuse: changing the period also (re)activates the timer.
        started = (xTimerChangePeriod(s_retry_tmr[slot], period, 0) == pdPASS);
    }
    if (!started) {
        ESP_LOGW(TAG, "retry timer start failed ieee=0x%016llx — re-queue now",
                 (unsigned long long)ieee);
        portENTER_CRITICAL(&s_retry_mux);
        s_retry_busy[slot] = false;
        portEXIT_CRITICAL(&s_retry_mux);
        if (s_q) (void)xQueueSend(s_q, &ieee, 0);
        return;
    }
    // First retry logs at INFO so the operator sees the failure entered a
    // retry cycle. Subsequent retries drop to DEBUG to avoid spamming a
    // steady-state log once per backoff interval on a flaky device.
    if (attempts <= 1) {
        ESP_LOGI(TAG, "configure retry scheduled ieee=0x%016llx in %lus (attempt %u/%u)",
                 (unsigned long long)ieee, (unsigned long)secs, attempts, MAX_ATTEMPTS);
    } else {
        ESP_LOGD(TAG, "configure retry scheduled ieee=0x%016llx in %lus (attempt %u/%u)",
                 (unsigned long long)ieee, (unsigned long)secs, attempts, MAX_ATTEMPTS);
    }
}

static void task_configure(void*) {
    uint64_t ieee = 0;
    for (;;) {
        if (xQueueReceive(s_q, &ieee, portMAX_DELAY) != pdTRUE) continue;

        // Snapshot device under lock. Worker never calls matcher/configure
        // while holding the pool mutex — those can take seconds.
        ZapDevice snap{};
        bool have_dev = false;
        uint8_t cur_state   = 0;
        uint8_t cur_support = 0;
        uint8_t attempts    = 0;
        zigbee_pool_lock();
        ZapDevice* d = pool_find_by_ieee(ieee);
        if (d) {
            snap        = *d;
            cur_state   = d->configure_state;
            cur_support = d->support_state;
            attempts    = d->configure_attempts;
            have_dev    = true;
        }
        zigbee_pool_unlock();

        if (!have_dev) continue;

        // Dedup: already configured → nothing to do. Prevents duplicate binds
        // on rejoin (QWEN guidance: some devices have 4–16-slot binding
        // tables and return STATUS_TABLE_FULL on duplicates).
        if (cur_state == (uint8_t)ConfigureState::DONE) {
            ESP_LOGD(TAG, "skip configure ieee=0x%016llx — already DONE",
                     (unsigned long long)ieee);
            continue;
        }

        // Require a converter binding before attempting configure.
        if (cur_support != (uint8_t)SupportState::MATCHED) {
            ESP_LOGD(TAG, "skip configure ieee=0x%016llx — support=%u",
                     (unsigned long long)ieee, cur_support);
            continue;
        }

        if (!zhac_adapter_has_def(snap.ieee_addr, snap.model_id,
                                    snap.manufacturer_name)) {
            // Support said MATCHED but ZHC lookup now fails — stale
            // definition. Downgrade and give up; late identity path will
            // re-promote.
            zigbee_pool_lock();
            if (auto* dd = pool_find_by_ieee(ieee)) {
                dd->support_state   = (uint8_t)SupportState::UNMATCHED;
                dd->configure_state = (uint8_t)ConfigureState::PENDING;
                ZapDevice s2 = *dd;
                zigbee_pool_unlock();
                zap_store_mark_dirty(&s2, ZAP_PERSIST_LOW);
            } else {
                zigbee_pool_unlock();
            }
            continue;
        }

        // Mark IN_PROGRESS and run. Not persisted — transient state.
        zigbee_pool_lock();
        if (auto* dd = pool_find_by_ieee(ieee)) {
            dd->configure_state = (uint8_t)ConfigureState::IN_PROGRESS;
        }
        zigbee_pool_unlock();

        (void)attempts;
        const bool ok = zhac_adapter_configure(snap.ieee_addr,
                                                snap.nwk_addr,
                                                snap.model_id,
                                                snap.manufacturer_name);

        // Commit outcome.
        uint8_t next_attempts = ok ? 0 : (uint8_t)(attempts + 1);
        uint8_t next_state = ok ? (uint8_t)ConfigureState::DONE
                                : (uint8_t)ConfigureState::FAILED;
        zigbee_pool_lock();
        auto* dd = pool_find_by_ieee(ieee);
        if (dd) {
            dd->configure_state    = next_state;
            dd->configure_attempts = next_attempts;
            ZapDevice s2 = *dd;
            zigbee_pool_unlock();
            zap_store_mark_dirty(&s2, ZAP_PERSIST_LOW);
        } else {
            zigbee_pool_unlock();
        }

        if (ok) {
            ESP_LOGI(TAG, "configure DONE ieee=0x%016llx model='%s' mfg='%s'",
                     (unsigned long long)ieee,
                     snap.model_id, snap.manufacturer_name);
            // Any per-device quirks (Tuya magic packet, miboxerSetZones,
            // tuyaSetup, etc.) now live in the device's config_steps[]
            // array and fire inside zhac_adapter_configure above.
        } else if (next_attempts < MAX_ATTEMPTS) {
            schedule_retry(ieee, next_attempts);
        } else {
            ESP_LOGE(TAG, "configure FAILED ieee=0x%016llx after %u attempts — "
                     "giving up until rejoin",
                     (unsigned long long)ieee, MAX_ATTEMPTS);
        }
    }
}

void zigbee_configure_enqueue(uint64_t ieee) {
    if (!s_q) return;
    if (xQueueSend(s_q, &ieee, 0) != pdTRUE) {
        // Always-on routers (Tuya dimmers, Hue) never rejoin once joined,
        // so the previous "retry on next rejoin" path stranded them
        // permanently in ConfigureState::PENDING. Schedule a 1 s retry
        // (attempts=0 → BACKOFF_SECS[0]=1) so the work is eventually
        // serviced once the queue drains.
        ESP_LOGW(TAG, "configure queue full ieee=0x%016llx — retry in 1s",
                 (unsigned long long)ieee);
        schedule_retry(ieee, 0);
    }
}

void zigbee_configure_init() {
    if (s_q) return;  // idempotent
    s_q = xQueueCreate(CONFIGURE_QUEUE_DEPTH, sizeof(uint64_t));
    configASSERT(s_q);
    xTaskCreate(task_configure, "zb_configure", zhac::stack::kZbConfigure, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "configure queue ready (depth=%u max_attempts=%u)",
             CONFIGURE_QUEUE_DEPTH, MAX_ATTEMPTS);
}
