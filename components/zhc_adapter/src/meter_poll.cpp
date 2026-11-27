// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "meter_poll.hpp"

namespace zhac_meter {

bool stays_awake(std::uint8_t power_source, std::uint8_t logical_type) {
    const std::uint8_t ps = power_source & 0x7F;
    return ps != 0x03 && (logical_type == 1 || ps == 0x01);
}

std::size_t reads_for(std::uint8_t flags, Read out[2]) {
    std::size_t n = 0;
    if (flags & kElectrical) {
        // rmsVoltage 0x0505, rmsCurrent 0x0508, activePower 0x050B
        out[n++] = Read{0x0B04, 3, {0x05, 0x05, 0x08, 0x05, 0x0B, 0x05}};
    }
    if (flags & kMetering) {
        out[n++] = Read{0x0702, 1, {0x00, 0x00, 0, 0, 0, 0}};   // currentSummDelivered
    }
    return n;
}

std::uint32_t stagger(std::uint64_t ieee, std::uint32_t interval_ms) {
    if (interval_ms == 0) return 0;
    const std::uint64_t h = ieee * 0x9E3779B97F4A7C15ull;   // Fibonacci hash
    return static_cast<std::uint32_t>((h >> 32) % interval_ms);
}

namespace {

// The interval times the backoff, capped, plus the device's own jitter.
std::uint32_t ping_wait(std::uint64_t ieee, std::uint32_t interval_ms, std::uint32_t backoff_x2) {
    const std::uint64_t ms = static_cast<std::uint64_t>(interval_ms) * backoff_x2 / 2;
    return static_cast<std::uint32_t>(ms < kPingMaxMs ? ms : kPingMaxMs) + stagger(ieee, kPingJitterMs);
}

}  // namespace

Scheduler::Slot* Scheduler::find(std::uint64_t ieee) {
    for (auto& s : slots_) if (s.ieee == ieee) return &s;
    return nullptr;
}

bool Scheduler::add(std::uint64_t ieee, std::uint16_t nwk, std::uint8_t flags,
                    std::uint32_t now_ms, std::uint32_t interval_ms,
                    bool awake, std::uint8_t ping_ep, std::uint32_t ping_interval_ms) {
    if (ieee == 0) return false;
    Slot* s = find(ieee);
    if (!s) {
        s = find(0);
        if (!s) return false;
        *s = Slot{};
        s->ieee        = ieee;
        s->due_ms      = now_ms + stagger(ieee, interval_ms);
        s->backoff_x2  = 2;
        s->ping_due_ms = now_ms + ping_wait(ieee, ping_interval_ms, 2);
        s->heard_ms    = now_ms;
    }
    s->nwk = nwk;
    s->flags = flags;
    s->watched = ping_interval_ms != 0;
    s->awake = awake && s->watched;
    s->ping_ep = ping_ep ? ping_ep : 1;
    s->seen = true;
    return true;
}

bool Scheduler::known(std::uint64_t ieee) const {
    if (ieee == 0) return false;
    for (const auto& s : slots_) if (s.ieee == ieee) return true;
    return false;
}

void Scheduler::forget(std::uint64_t ieee) {
    if (ieee == 0) return;
    if (Slot* s = find(ieee)) *s = Slot{};
}

void Scheduler::begin_scan() {
    for (auto& s : slots_) s.seen = false;
}

bool Scheduler::refresh(std::uint64_t ieee, std::uint16_t nwk, bool awake) {
    Slot* s = ieee ? find(ieee) : nullptr;
    if (!s) return false;
    s->nwk = nwk;
    s->seen = true;
    awake = awake && s->watched;
    if (s->awake != awake) {   // pinged from now on, or no longer: a new start for the ping
        s->awake = awake;
        s->waiting = false;
        s->backoff_x2 = 2;
    }
    return true;
}

void Scheduler::end_scan() {
    for (auto& s : slots_) if (s.ieee && !s.seen) s = Slot{};
}

std::size_t Scheduler::take_due(std::uint32_t now_ms, Due* out, std::size_t max) {
    std::size_t n = 0;
    for (const auto& s : slots_) {
        if (n >= max) break;
        if (!s.ieee || !s.flags) continue;
        if (static_cast<std::int32_t>(now_ms - s.due_ms) < 0) continue;
        out[n++] = Due{s.ieee, s.nwk, s.flags, 0};
    }
    return n;
}

bool Scheduler::polled(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms,
                       bool ok) {
    Slot* s = find(ieee);
    if (!s) return false;
    s->due_ms = now_ms + interval_ms;
    if (ok) { s->warned = false; return false; }
    if (s->warned) return false;
    s->warned = true;
    return true;
}

bool Scheduler::heard(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms) {
    Slot* s = ieee ? find(ieee) : nullptr;
    if (!s) return false;
    s->heard_ms = now_ms;
    if (!s->awake) {   // not pinged: only an offline verdict to take back
        const bool back = s->offline;
        s->offline = false;
        return back;
    }
    // Offline, pinged already, or due and still waiting for its turn. Offline counts on its own: at the longest
    // interval the backoff cannot grow, and a device back from offline must be reported (research 2026-11-18 B7).
    const bool silence = s->offline || s->backoff_x2 > 2 || static_cast<std::int32_t>(now_ms - s->ping_due_ms) >= 0;
    s->waiting = s->offline = false;
    s->backoff_x2 = 2;
    s->ping_due_ms = now_ms + ping_wait(ieee, interval_ms, 2);
    return silence;
}

std::size_t Scheduler::take_ping_due(std::uint32_t now_ms, Due* out, std::size_t max) {
    std::size_t n = 0;
    for (const auto& s : slots_) {
        if (n >= max) break;
        if (!s.ieee || !s.awake) continue;
        if (static_cast<std::int32_t>(now_ms - s.ping_due_ms) < 0) continue;
        out[n++] = Due{s.ieee, s.nwk, s.flags, s.ping_ep};
    }
    return n;
}

bool Scheduler::pinged(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms) {
    Slot* s = find(ieee);
    if (!s || !s->awake) return false;
    const bool missed = s->backoff_x2 == 3;   // set by the previous ping, not reset by an answer
    if (static_cast<std::uint64_t>(interval_ms) * s->backoff_x2 / 2 < kPingMaxMs) {
        s->backoff_x2 = s->backoff_x2 == 2 ? 3 : s->backoff_x2 * 2;
    }
    s->ping_due_ms = now_ms + ping_wait(ieee, interval_ms, s->backoff_x2);
    s->ping_sent_ms = now_ms;
    s->waiting = true;
    return missed;
}

std::size_t Scheduler::take_offline(std::uint32_t now_ms, std::uint64_t* out, std::size_t max) {
    std::size_t n = 0;
    for (auto& s : slots_) {
        if (n >= max) break;
        if (!s.ieee || !s.watched || s.offline) continue;
        const std::uint32_t quiet = now_ms - s.heard_ms;
        const bool gone = s.awake ? s.waiting && now_ms - s.ping_sent_ms >= kOfflineMs && quiet >= kActiveOfflineMs
                                  : quiet >= kPassiveOfflineMs;
        if (!gone) continue;
        s.offline = true;
        out[n++] = s.ieee;
    }
    return n;
}

void Scheduler::not_sent(std::uint64_t ieee) {
    if (Slot* s = find(ieee)) s->waiting = false;
}

void Scheduler::unreachable(std::uint64_t ieee, std::uint32_t now_ms) {
    Slot* s = ieee ? find(ieee) : nullptr;
    if (!s || !s->awake || s->offline) return;
    s->waiting = true;
    s->ping_sent_ms = now_ms - kOfflineMs;   // as if pinged long enough ago
}

bool Scheduler::offline(std::uint64_t ieee) const {
    for (const auto& s : slots_) {
        if (s.ieee == ieee) return ieee && s.offline;
    }
    return false;
}

}  // namespace zhac_meter
