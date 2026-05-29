// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_worker.cpp — single-owner UART TX path for SREQ calls.
//
// Architecture:
//   - One FreeRTOS worker task owns all UART transmits.
//   - Callers post ZnpRequest onto s_request_q; worker dequeues one at a time.
//   - Replies land in a preallocated pool (s_slots[REPLY_POOL_N]); caller
//     synchronises on a per-slot binary semaphore. No per-call allocations.
//   - The RX task classifies incoming frames and, for matching SRSP or
//     SYS_RESET_IND, pushes a WorkerWake onto s_wake_q. Worker sits in a
//     single xQueueReceive while waiting — no spin, no polling.
//
// Late SRSP policy (see README):
//   - snapshot_active() reports whether a request is currently in-flight.
//   - If RX calls deliver_srsp() with no active request, we log "late SRSP",
//     bump stats.late_srsp, and drop. No caller ever sees a resurrected
//     timed-out reply.
//   - If RX calls deliver_srsp() but the SRSP doesn't match the active
//     request (different subsystem / cmd1), we log "unexpected SRSP",
//     bump stats.unexpected_srsp, and drop. The active request will
//     eventually time out, which is the correct outcome.
//
// Why the slot pool exists:
//   caller and worker are different tasks. The worker must hand exactly one
//   ZnpReply back to exactly one caller without any shared mutable state.
//   Earlier design created a new FreeRTOS queue per call (xQueueCreate +
//   vQueueDelete). The pool replaces that with REPLY_POOL_N preallocated
//   slots, each owning its own binary semaphore + ZnpReply value. A counting
//   semaphore over the pool enforces back-pressure when all slots are in use.

#include "znp_internal.h"
#include "znp_driver.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "metrics/metrics_macros.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <cstring>
#include "task_stacks.h"

static const char* TAG = "znp_worker";

// ── Reply-slot pool ───────────────────────────────────────────────────────
static constexpr size_t REPLY_POOL_N = 4;
struct ReplySlot {
    SemaphoreHandle_t sem;   // binary: given by worker, taken by caller
    ZnpReply          reply;
};
static ReplySlot         s_slots[REPLY_POOL_N];
static bool              s_slot_in_use[REPLY_POOL_N] = { false };
static SemaphoreHandle_t s_slot_free_count = nullptr;    // counting, N initial
static portMUX_TYPE      s_slot_mux = portMUX_INITIALIZER_UNLOCKED;

static int acquire_slot(uint32_t wait_ms) {
    // Counting semaphore gates pool exhaustion; mutex just picks which slot.
    if (xSemaphoreTake(s_slot_free_count, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return -1;
    }
    int idx = -1;
    portENTER_CRITICAL(&s_slot_mux);
    for (size_t i = 0; i < REPLY_POOL_N; i++) {
        if (!s_slot_in_use[i]) { s_slot_in_use[i] = true; idx = (int)i; break; }
    }
    portEXIT_CRITICAL(&s_slot_mux);
    if (idx < 0) {
        // The counting semaphore should make this unreachable; the
        // explicit branch keeps -O2 from warning on the array index
        // and gives an actionable failure mode if invariants ever drift.
        xSemaphoreGive(s_slot_free_count);
        return -1;
    }
    // Drain any stale give that might linger from a previous reuse of the
    // slot (defensive: should never fire given the mutex + counting sem).
    xSemaphoreTake(s_slots[idx].sem, 0);
    return idx;
}

static void release_slot(int idx) {
    portENTER_CRITICAL(&s_slot_mux);
    s_slot_in_use[idx] = false;
    portEXIT_CRITICAL(&s_slot_mux);
    xSemaphoreGive(s_slot_free_count);
}

// ── Request / wake types ──────────────────────────────────────────────────
struct ZnpRequest {
    ZnpFrame req;
    uint8_t  slot_idx;
    uint32_t timeout_ms;
};

enum class WorkerEvent : uint8_t { Srsp, ResetInd };
struct WorkerWake {
    WorkerEvent type;
    ZnpFrame    frame;
};

static QueueHandle_t s_request_q = nullptr;
static QueueHandle_t s_wake_q    = nullptr;

// ── Active-request snapshot ───────────────────────────────────────────────
static portMUX_TYPE s_active_mux    = portMUX_INITIALIZER_UNLOCKED;
static bool         s_active        = false;
static uint8_t      s_active_cmd0   = 0;
static uint8_t      s_active_cmd1   = 0;

static void set_active(uint8_t cmd0, uint8_t cmd1) {
    portENTER_CRITICAL(&s_active_mux);
    s_active = true;
    s_active_cmd0 = cmd0;
    s_active_cmd1 = cmd1;
    portEXIT_CRITICAL(&s_active_mux);
}
static void clear_active() {
    portENTER_CRITICAL(&s_active_mux);
    s_active = false;
    portEXIT_CRITICAL(&s_active_mux);
}
static bool snapshot_active(uint8_t& cmd0, uint8_t& cmd1) {
    bool present;
    portENTER_CRITICAL(&s_active_mux);
    present = s_active;
    cmd0 = s_active_cmd0;
    cmd1 = s_active_cmd1;
    portEXIT_CRITICAL(&s_active_mux);
    return present;
}

// ── RX → worker delivery ──────────────────────────────────────────────────
void znp_worker_deliver_srsp(const ZnpFrame& f) {
    uint8_t a0 = 0, a1 = 0;
    if (!snapshot_active(a0, a1)) {
        ESP_LOGW(TAG, "late SRSP (no active req) cmd0=0x%02x cmd1=0x%02x",
                 f.cmd0, f.cmd1);
        znp_stats_bump(ZnpStat::LateSrsp);
        return;
    }
    if (!znp_is_expected_srsp(a0, a1, f.cmd0, f.cmd1)) {
        ESP_LOGW(TAG, "unexpected SRSP cmd0=0x%02x cmd1=0x%02x "
                      "(waiting cmd0=0x%02x cmd1=0x%02x)",
                 f.cmd0, f.cmd1, a0, a1);
        znp_stats_bump(ZnpStat::UnexpectedSrsp);
        return;
    }
    WorkerWake w{ WorkerEvent::Srsp, f };
    if (xQueueSend(s_wake_q, &w, 0) != pdTRUE) {
        // Wake queue was already full — should be rare with depth 4 but
        // a silent loss here would look like a spurious SRSP timeout on
        // the caller. Log + count so we can see it.
        ESP_LOGW(TAG, "wake_q full — SRSP dropped cmd0=0x%02x cmd1=0x%02x",
                 f.cmd0, f.cmd1);
        znp_stats_bump(ZnpStat::BadFrame);
    }
}

void znp_worker_deliver_reset_ind(const ZnpFrame& f) {
    if (!s_wake_q) return;
    WorkerWake w{ WorkerEvent::ResetInd, f };
    if (xQueueSend(s_wake_q, &w, 0) != pdTRUE) {
        ESP_LOGW(TAG, "wake_q full — RESET_IND dropped");
    }
}

// ── Worker task body ──────────────────────────────────────────────────────
static void write_reply(uint8_t slot_idx, ZnpStatus status,
                        const ZnpFrame* srsp) {
    ReplySlot& s = s_slots[slot_idx];
    s.reply.status = status;
    if (srsp) s.reply.srsp = *srsp;
    xSemaphoreGive(s.sem);
}

static bool tx_frame(const ZnpFrame& f) {
    uint8_t buf[MT_MAX_FRAME];
    const size_t n = znp_mt_encode(f, buf, sizeof(buf));
    if (n == 0) {
        znp_stats_bump(ZnpStat::TxError);
        return false;
    }
    // Wire trace. Off by default; operator flips with znp_set_wire_trace(true).
    // Uses INFO level so it works regardless of CONFIG_LOG_MAXIMUM_LEVEL.
    // Dumps the full on-wire bytes (SOF..FCS) so it's reproducible against
    // external captures.
    if (znp_get_wire_trace()) {
        ESP_LOGI("znp_wire", "TX cmd0=0x%02x cmd1=0x%02x len=%u",
                 f.cmd0, f.cmd1, (unsigned)f.len);
        ESP_LOG_BUFFER_HEX_LEVEL("znp_wire", buf, n, ESP_LOG_INFO);
    }
    const int written = uart_write_bytes(znp_uart_port, buf, n);
    if (written != (int)n) {
        ESP_LOGE(TAG, "uart_write_bytes short write %d/%u",
                 written, (unsigned)n);
        znp_stats_bump(ZnpStat::TxError);
        return false;
    }
    znp_stats_bump(ZnpStat::TxSreq);
    return true;
}

static void drain_wake_q() {
    WorkerWake tmp;
    while (xQueueReceive(s_wake_q, &tmp, 0) == pdTRUE) { /* drop */ }
}

static void handle_request(const ZnpRequest& r) {
    const auto state = znp_get_state();
    if (state == ZnpTransportState::Down || state == ZnpTransportState::Error) {
        write_reply(r.slot_idx, ZnpStatus::TRANSPORT_DOWN, nullptr);
        return;
    }

    drain_wake_q();
    set_active(r.req.cmd0, r.req.cmd1);  // set BEFORE tx: RX may be fast

    if (!tx_frame(r.req)) {
        clear_active();
        write_reply(r.slot_idx, ZnpStatus::UART_TX_ERROR, nullptr);
        return;
    }

    WorkerWake wake;
    const BaseType_t got = xQueueReceive(s_wake_q, &wake,
                                          pdMS_TO_TICKS(r.timeout_ms));
    clear_active();

    if (got != pdTRUE) {
        znp_stats_bump(ZnpStat::Timeout);
        ESP_LOGE(TAG, "SRSP timeout cmd0=0x%02x cmd1=0x%02x",
                 r.req.cmd0, r.req.cmd1);
        write_reply(r.slot_idx, ZnpStatus::TIMEOUT, nullptr);
        return;
    }

    if (wake.type == WorkerEvent::ResetInd) {
        ESP_LOGW(TAG, "SYS_RESET_IND mid-call cmd0=0x%02x cmd1=0x%02x",
                 r.req.cmd0, r.req.cmd1);
        znp_stats_bump(ZnpStat::Recovery);
        write_reply(r.slot_idx, ZnpStatus::RESET_DURING_CALL, nullptr);
        return;
    }

    znp_stats_bump(ZnpStat::RxSrsp);
    znp_state_note_ok();   // successful SRSP clears any Recovering state
    write_reply(r.slot_idx, ZnpStatus::OK, &wake.frame);
}

static void znp_worker_task(void*) {
    ESP_LOGI(TAG, "worker task started");
    ZnpRequest r;
    while (true) {
        if (xQueueReceive(s_request_q, &r, portMAX_DELAY) == pdTRUE) {
            handle_request(r);
        }
    }
}

void znp_worker_task_start() {
    s_request_q = xQueueCreate(4, sizeof(ZnpRequest));
    // Depth 4 instead of 1: RX can briefly queue a second SRSP or a
    // RESET_IND while the worker is still dispatching the previous wake,
    // and we'd rather log/count than silently drop (QWEN §17, CODEX §2).
    s_wake_q    = xQueueCreate(4, sizeof(WorkerWake));
    configASSERT(s_request_q && s_wake_q);

    s_slot_free_count = xSemaphoreCreateCounting(REPLY_POOL_N, REPLY_POOL_N);
    configASSERT(s_slot_free_count);
    for (size_t i = 0; i < REPLY_POOL_N; i++) {
        s_slots[i].sem = xSemaphoreCreateBinary();
        configASSERT(s_slots[i].sem);
    }

    xTaskCreate(znp_worker_task, "znp_worker", zhac::stack::kZnpWorker, nullptr, 5, nullptr);
}

// ── Public API: znp_call ──────────────────────────────────────────────────
ZnpStatus znp_call(uint8_t cmd0, uint8_t cmd1,
                   const uint8_t* data, uint8_t data_len,
                   ZnpFrame& srsp_out, uint32_t timeout_ms) {
    _METRIC_TIMER_SCOPE(METRIC_ZNP_CALL);
    if (!s_request_q) return ZnpStatus::TRANSPORT_DOWN;
    if (data_len > ZNP_MAX_DATA_LEN) return ZnpStatus::INTERNAL_ERROR;

    // Acquire a reply slot. If the pool is saturated, wait up to the same
    // timeout the caller asked for. This provides natural back-pressure: if
    // 4 callers are already blocked on SRSPs, the 5th waits instead of
    // allocating unbounded memory.
    const int slot = acquire_slot(timeout_ms);
    if (slot < 0) return ZnpStatus::TRANSPORT_DOWN;

    ZnpRequest r{};
    r.req.cmd0 = cmd0;
    r.req.cmd1 = cmd1;
    r.req.len  = data_len;
    if (data_len && data) memcpy(r.req.data, data, data_len);
    r.slot_idx   = (uint8_t)slot;
    r.timeout_ms = timeout_ms;

    if (xQueueSend(s_request_q, &r, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        release_slot(slot);
        return ZnpStatus::TRANSPORT_DOWN;
    }

    // Worker guarantees exactly one give on the slot semaphore per accepted
    // request. Block forever here to avoid a race where the caller gives up
    // and releases the slot while the worker is about to write into it.
    xSemaphoreTake(s_slots[slot].sem, portMAX_DELAY);
    const ZnpReply reply = s_slots[slot].reply;
    release_slot(slot);

    if (reply.status == ZnpStatus::OK) srsp_out = reply.srsp;
    if (reply.status == ZnpStatus::TIMEOUT) {
        _METRIC_COUNTER_INC(METRIC_ZNP_TIMEOUTS_TOTAL, 1);
    }
    return reply.status;
}

ZnpStatus znp_call_retry(uint8_t cmd0, uint8_t cmd1,
                         const uint8_t* data, uint8_t data_len,
                         ZnpFrame& srsp_out,
                         uint32_t timeout_ms, int max_attempts) {
    ZnpStatus last = ZnpStatus::INTERNAL_ERROR;
    for (int i = 1; i <= max_attempts; i++) {
        last = znp_call(cmd0, cmd1, data, data_len, srsp_out, timeout_ms);
        if (last == ZnpStatus::OK) return last;
        // Fail fast on statuses that won't be fixed by retrying.
        if (last == ZnpStatus::TRANSPORT_DOWN ||
            last == ZnpStatus::INTERNAL_ERROR) {
            return last;
        }
        ESP_LOGW(TAG, "znp_call attempt %d/%d failed "
                      "cmd0=0x%02x cmd1=0x%02x status=%u",
                 i, max_attempts, cmd0, cmd1, (unsigned)last);
        // F43 (FINDINGS.md): back off between retries so a momentarily wedged
        // NCP isn't hammered with back-to-back commands. 25/50/100/200 ms,
        // capped at 400 ms; no delay after the final attempt.
        if (i < max_attempts) {
            uint32_t backoff_ms = (i <= 5) ? (25u << (i - 1)) : 400u;
            if (backoff_ms > 400u) backoff_ms = 400u;
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
    }
    return last;
}
