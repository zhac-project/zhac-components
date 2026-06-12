// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_rx.cpp — sole owner of uart_read_bytes for the ZNP port.
//
// The RX task feeds a stream parser one byte at a time. When a full frame
// is reconstructed, it's classified into SRSP / SYS_RESET_IND / AREQ and
// forwarded on the appropriate path:
//   - SRSP     → worker (via znp_worker_deliver_srsp, which also does the
//                matching + late/unexpected bookkeeping).
//   - RESET_IND → worker (so an in-flight caller can be woken with
//                RESET_DURING_CALL) AND the state machine transitions to Up.
//   - AREQ     → direct dispatch to subscriber list.
//
// Anything else (parse errors, bad FCS) is logged and counted inside the
// parser itself.

#include "znp_internal.h"
#include "znp_driver.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "task_stacks.h"

static const char* TAG = "znp_rx";

// MT type bits (top 3 bits of cmd0). Duplicated from the legacy driver for
// readability at the one place we need them.
static constexpr uint8_t MT_TYPE_MASK = 0xE0;
static constexpr uint8_t MT_SUBS_MASK = 0x1F;
static constexpr uint8_t TYPE_AREQ    = 0x40;
static constexpr uint8_t TYPE_SRSP    = 0x60;

static constexpr uint8_t ZNP_SUB_SYS  = 0x01;
static constexpr uint8_t ZNP_SUB_ZDO  = 0x05;
static constexpr uint8_t SYS_RESET_IND_CMD1 = 0x80;

// ── AREQ dedup ───────────────────────────────────────────────────────────
// TI's MT protocol has no sequence number on AREQ frames. When the radio
// occasionally retransmits identical ZDO response indications (observed on
// noisy RF environments and during fast re-joins) the upper layers see
// duplicate interview answers, which waste work and can race on state
// machines. Scoped to ZDO response cmd1 values where rapid legitimate
// duplicates are impossible:
//
//   0x82 NODE_DESC_RSP
//   0x84 SIMPLE_DESC_RSP
//   0x85 ACTIVE_EP_RSP
//   0xCA TC_DEV_IND  (the rapid double-burst this dedup actually targets —
//                      Xiaomi devices fire two TC_DEV_IND on wake, see
//                      zigbee_interview.cpp; AF/attribute-report AREQs are
//                      left alone — they can legitimately repeat, e.g.
//                      rapid contact open/close.)
//
// §4 (FINDINGS.md): this list previously named 0xA1 as "TC_DEV_IND", but
// 0xA1 is ZDO_BIND_RSP — TC_DEV_IND is 0xCA (see the AREQ registrations in
// zigbee_interview_init). The off-by-target meant the device-announce
// double-bursts we MEANT to dedup were never deduped, while two distinct
// BIND_RSPs (e.g. two binds with identical TABLE_FULL status) landing
// within 200 ms were wrongly dropped — losing a bind outcome.
//
// Dedup key: (cmd0, cmd1, FNV-1a hash of data[0..len]) within a 200 ms
// window. 8-slot ring, wraps on overflow.
static constexpr uint8_t  DUP_RING_SIZE = 8;
static constexpr uint32_t DUP_WINDOW_MS = 200;

struct DupEntry {
    uint8_t  cmd0;
    uint8_t  cmd1;
    uint32_t hash;
    uint32_t ts_ms;
};
static DupEntry s_dup_ring[DUP_RING_SIZE] = {};
static uint8_t  s_dup_head = 0;

static uint32_t fnv1a(const uint8_t* data, uint8_t len) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static bool zdo_areq_is_in_scope(uint8_t cmd1) {
    // 0x82 NODE_DESC_RSP, 0x84 SIMPLE_DESC_RSP, 0x85 ACTIVE_EP_RSP,
    // 0xCA TC_DEV_IND (NOT 0xA1 ZDO_BIND_RSP — see header comment, §4).
    return cmd1 == 0x82 || cmd1 == 0x84 || cmd1 == 0x85 || cmd1 == 0xCA;
}

// Returns true if the caller should drop the frame as a duplicate.
static bool areq_is_dup(const ZnpFrame& f) {
    const uint8_t subs = f.cmd0 & MT_SUBS_MASK;
    if (subs != ZNP_SUB_ZDO) return false;
    if (!zdo_areq_is_in_scope(f.cmd1)) return false;

    const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    const uint32_t h   = fnv1a(f.data, f.len);

    for (uint8_t i = 0; i < DUP_RING_SIZE; i++) {
        const DupEntry& e = s_dup_ring[i];
        if (e.ts_ms == 0) continue;
        if (e.cmd0 == f.cmd0 && e.cmd1 == f.cmd1 && e.hash == h
            && (now - e.ts_ms) < DUP_WINDOW_MS) {
            return true;
        }
    }
    s_dup_ring[s_dup_head] = { f.cmd0, f.cmd1, h, now };
    s_dup_head = (uint8_t)((s_dup_head + 1) % DUP_RING_SIZE);
    return false;
}

static void on_frame(const ZnpFrame& f, void* /*ctx*/) {
    // Wire trace. Same tag as TX path so one znp_set_wire_trace(true) call
    // captures both directions.
    if (znp_get_wire_trace()) {
        ESP_LOGI("znp_wire", "RX cmd0=0x%02x cmd1=0x%02x len=%u",
                 f.cmd0, f.cmd1, (unsigned)f.len);
        // §4: redact the payload of a key NV write on this direction too
        // (some stacks echo it; the SREQ may also be observed here under
        // loopback). Never hex-dump the network key.
        if (f.len > 0) {
            if (znp_wire_is_sensitive(f.cmd0, f.cmd1, f.data, f.len)) {
                ESP_LOGI("znp_wire", "RX payload REDACTED (network key NV write)");
            } else {
                ESP_LOG_BUFFER_HEX_LEVEL("znp_wire", f.data, f.len, ESP_LOG_INFO);
            }
        }
    }

    const uint8_t type = f.cmd0 & MT_TYPE_MASK;
    const uint8_t subs = f.cmd0 & MT_SUBS_MASK;

    if (type == TYPE_SRSP) {
        znp_worker_deliver_srsp(f);
        return;
    }

    if (type == TYPE_AREQ) {
        if (subs == ZNP_SUB_SYS && f.cmd1 == SYS_RESET_IND_CMD1) {
            // SYS_RESET_IND: NCP has just booted (or been reset). Treat as a
            // transport event in addition to any in-flight call. The state
            // machine (znp_state.cpp) decides whether this is a normal first
            // boot, an unexpected reset (→ Recovering), or part of a crash
            // burst (→ Error).
            ESP_LOGI(TAG, "SYS_RESET_IND");
            znp_stats_bump(ZnpStat::Reset);
            znp_state_note_reset();
            znp_worker_deliver_reset_ind(f);
            // Also dispatch to AREQ subscribers so zigbee_mgr can observe the
            // reset it is explicitly waiting for during coordinator startup.
            znp_areq_dispatch(f);
            return;
        }
        if (areq_is_dup(f)) {
            znp_stats_bump(ZnpStat::DuplicateAreq);
            ESP_LOGW(TAG, "dedup: dropped duplicate ZDO AREQ "
                     "cmd0=0x%02x cmd1=0x%02x len=%u",
                     f.cmd0, f.cmd1, (unsigned)f.len);
            return;
        }
        znp_stats_bump(ZnpStat::RxAreq);
        znp_areq_dispatch(f);
        return;
    }

    ESP_LOGW(TAG, "unexpected frame type cmd0=0x%02x cmd1=0x%02x",
             f.cmd0, f.cmd1);
    znp_stats_bump(ZnpStat::BadFrame);
}

static void znp_rx_task(void*) {
    ESP_LOGI(TAG, "rx task started");
    MtStreamParser parser;
    parser.reset();
    uint8_t buf[256];
    // Q58 (QWEN_FINDINGS triage): subscribe to the task watchdog so a wedged RX
    // loop reboots the host (best effort — no-op if the TWDT isn't enabled).
    // The 50 ms read timeout below feeds the WDT each loop even when idle.
    // (The worker task intentionally blocks on portMAX_DELAY — idle there is
    // normal, so it is NOT WDT-subscribed.)
    esp_task_wdt_add(NULL);
    while (true) {
        // Q32 (QWEN_FINDINGS triage): drain UART events; on an RX overrun the
        // dropped bytes corrupt the in-flight MT frame, so flush the driver
        // buffers + queued events and resync the parser. uart_read_bytes still
        // owns the data path (the event queue is a separate notification
        // channel) — keeping the proven read loop intact.
        if (znp_uart_evt_q) {
            uart_event_t ev;
            while (xQueueReceive(znp_uart_evt_q, &ev, 0) == pdTRUE) {
                if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                    ESP_LOGW(TAG, "UART RX overrun (event %d) — flush + parser resync",
                             (int)ev.type);
                    uart_flush_input(znp_uart_port);
                    xQueueReset(znp_uart_evt_q);
                    parser.reset();
                    break;
                }
            }
        }
        const int n = uart_read_bytes(znp_uart_port, buf, sizeof(buf),
                                       pdMS_TO_TICKS(50));
        esp_task_wdt_reset();   // Q58: fed each loop (data or 50 ms timeout)
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) parser.feed(buf[i], on_frame, nullptr);
    }
}

void znp_rx_task_start() {
    xTaskCreate(znp_rx_task, "znp_rx", zhac::stack::kZnpRx, nullptr, 6, nullptr);
}
