// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "meter_poll.hpp"

namespace zhac_meter {

bool mains_powered(std::uint8_t power_source) {
    switch (power_source & 0x7F) {
        case 0x01: case 0x02: case 0x04: case 0x05: case 0x06: return true;
        default: return false;
    }
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

Scheduler::Slot* Scheduler::find(std::uint64_t ieee) {
    for (auto& s : slots_) if (s.ieee == ieee) return &s;
    return nullptr;
}

bool Scheduler::add(std::uint64_t ieee, std::uint16_t nwk, std::uint8_t flags,
                    std::uint32_t now_ms, std::uint32_t interval_ms) {
    if (ieee == 0) return false;
    Slot* s = find(ieee);
    if (!s) {
        s = find(0);
        if (!s) return false;
        *s = Slot{ieee, now_ms + stagger(ieee, interval_ms), nwk, flags, true};
        return true;
    }
    s->nwk = nwk;
    s->flags = flags;
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

bool Scheduler::refresh(std::uint64_t ieee, std::uint16_t nwk) {
    Slot* s = ieee ? find(ieee) : nullptr;
    if (!s) return false;
    s->nwk = nwk;
    s->seen = true;
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
        out[n++] = Due{s.ieee, s.nwk, s.flags};
    }
    return n;
}

void Scheduler::polled(std::uint64_t ieee, std::uint32_t now_ms, std::uint32_t interval_ms) {
    if (Slot* s = find(ieee)) s->due_ms = now_ms + interval_ms;
}

}  // namespace zhac_meter
