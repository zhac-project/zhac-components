// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// znp_state.cpp — transport state machine + diagnostic counters. Every
// mutation is behind a short critical section so callers can bump counters
// from ISR-adjacent contexts safely.

#include "znp_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char* TAG = "znp_state";

static ZnpTransportState s_state = ZnpTransportState::Down;
static ZnpTransportStats s_stats = {};
static portMUX_TYPE      s_mux   = portMUX_INITIALIZER_UNLOCKED;

// Wire-level packet trace flag. Lives here alongside the other transport
// flags so all runtime knobs for the ZNP side are in one place.
static volatile bool s_wire_trace = false;

void znp_set_wire_trace(bool enabled) {
    s_wire_trace = enabled;
    ESP_LOGI(TAG, "wire trace %s", enabled ? "ON" : "off");
}
bool znp_get_wire_trace() { return s_wire_trace; }

// Rolling timestamps of the last N resets, used to decide whether we're in
// a crash loop. N=3 + window=10s means: three resets inside ten seconds
// escalates the transport to Error. The window is short enough that a true
// crash loop is caught within seconds, long enough that a one-off reset
// during normal operation doesn't escalate.
static constexpr size_t RESET_BURST_N        = 3;
// F13 (FINDINGS.md): was 30 s, which contradicted the documented 10 s
// window above and the log line below — an NCP rebooting every ~11 s never
// tripped the burst detector. Now matches the intended 10 s.
static constexpr int64_t RESET_BURST_US      = 10LL * 1000 * 1000;  // 10 s
static int64_t s_reset_ts[RESET_BURST_N] = { 0 };
static size_t  s_reset_ts_next = 0;

static const char* name_of(ZnpTransportState s) {
    switch (s) {
        case ZnpTransportState::Down:       return "Down";
        case ZnpTransportState::Booting:    return "Booting";
        case ZnpTransportState::Init:       return "Init";
        case ZnpTransportState::Up:         return "Up";
        case ZnpTransportState::Recovering: return "Recovering";
        case ZnpTransportState::Error:      return "Error";
    }
    return "?";
}

void znp_state_set(ZnpTransportState s) {
    ZnpTransportState prev;
    portENTER_CRITICAL(&s_mux);
    prev = s_state;
    s_state = s;
    portEXIT_CRITICAL(&s_mux);
    if (prev != s) ESP_LOGI(TAG, "%s -> %s", name_of(prev), name_of(s));
}

ZnpTransportState znp_get_state() {
    portENTER_CRITICAL(&s_mux);
    auto s = s_state;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

ZnpTransportStats znp_get_stats() {
    ZnpTransportStats out;
    portENTER_CRITICAL(&s_mux);
    out = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return out;
}

// Recovery / escalation logic. Called from the RX task on every
// SYS_RESET_IND — the one place we know the NCP just booted.
void znp_state_note_reset() {
    const int64_t now = esp_timer_get_time();

    ZnpTransportState prev;
    ZnpTransportState next;
    int64_t oldest = 0;

    portENTER_CRITICAL(&s_mux);
    prev = s_state;

    // Normal first-boot transition: Down/Init/Booting → Up.
    if (prev == ZnpTransportState::Down ||
        prev == ZnpTransportState::Booting ||
        prev == ZnpTransportState::Init) {
        s_state = ZnpTransportState::Up;
        next = s_state;
        // Don't count this as a "burst" event.
        portEXIT_CRITICAL(&s_mux);
        if (prev != next) ESP_LOGI(TAG, "reset: first boot → Up");
        return;
    }

    // Any other state means the NCP reset unexpectedly. Record timestamp,
    // check whether the burst ring now covers a full N resets inside the
    // window, and escalate accordingly.
    s_reset_ts[s_reset_ts_next] = now;
    s_reset_ts_next = (s_reset_ts_next + 1) % RESET_BURST_N;

    oldest = s_reset_ts[s_reset_ts_next];   // after advance, this slot is the oldest
    const bool burst = (oldest != 0) && ((now - oldest) <= RESET_BURST_US);

    s_state = burst ? ZnpTransportState::Error : ZnpTransportState::Recovering;
    next = s_state;
    portEXIT_CRITICAL(&s_mux);

    if (burst) {
        ESP_LOGE(TAG, "reset burst: %u resets within %lld ms → Error",
                 (unsigned)RESET_BURST_N, (long long)(RESET_BURST_US / 1000));
    } else {
        ESP_LOGW(TAG, "unexpected reset → Recovering");
    }
}

// Every successful SRSP should clear a pending Recovering state. Not called
// from Error — Error is sticky until something external (e.g. a hard reset
// from zigbee_mgr's crash handler) puts the transport back through init.
void znp_state_note_ok() {
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    // F13 (FINDINGS.md): self-heal from Error too. A successful SRSP proves
    // the link is alive, so a wedged-then-recovered NCP no longer depends
    // solely on the external zigbee_mgr supervisor to clear the otherwise
    // sticky Error state.
    if (s_state == ZnpTransportState::Recovering ||
        s_state == ZnpTransportState::Error) {
        s_state = ZnpTransportState::Up;
        changed = true;
        // Clear the reset-burst ring — a successful recovery means the
        // previous transient resets shouldn't count against a future
        // isolated reset event (QWEN §18).
        for (size_t i = 0; i < RESET_BURST_N; i++) s_reset_ts[i] = 0;
        s_reset_ts_next = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) ESP_LOGI(TAG, "Recovering → Up (successful call, burst ring cleared)");
}

void znp_stats_bump(ZnpStat s) {
    portENTER_CRITICAL(&s_mux);
    switch (s) {
        case ZnpStat::TxSreq:          s_stats.tx_sreq_count++;   break;
        case ZnpStat::RxSrsp:          s_stats.rx_srsp_count++;   break;
        case ZnpStat::RxAreq:          s_stats.rx_areq_count++;   break;
        case ZnpStat::Reset:           s_stats.resets_seen++;     break;
        case ZnpStat::Timeout:         s_stats.timeouts++;        break;
        case ZnpStat::UnexpectedSrsp:  s_stats.unexpected_srsp++; break;
        case ZnpStat::LateSrsp:        s_stats.late_srsp++;       break;
        case ZnpStat::BadFrame:        s_stats.bad_frames++;      break;
        case ZnpStat::TxError:         s_stats.tx_errors++;       break;
        case ZnpStat::Recovery:        s_stats.recoveries++;      break;
        case ZnpStat::DuplicateAreq:   s_stats.duplicate_areqs++; break;
    }
    portEXIT_CRITICAL(&s_mux);
}
