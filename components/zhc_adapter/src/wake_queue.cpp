// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wake_queue.hpp"

#include <cstring>

namespace zhac_wake {

bool holdable(std::uint16_t cluster, const std::uint8_t* zcl, std::size_t len) {
    if (!zcl || len < 3) return false;
    const bool cluster_specific = (zcl[0] & 0x03) == 0x01;
    const std::size_t hdr = (zcl[0] & 0x04) ? 5 : 3;   // manufacturer code adds two bytes
    if (len < hdr) return false;
    const std::uint8_t cmd = zcl[hdr - 1];
    if (cluster == 0xEF00 && cluster_specific) return cmd == 0x00 || cmd == 0x03 || cmd == 0x04;
    if (!cluster_specific) return cmd == 0x02;        // Write Attributes
    return false;
}

bool Queue::hold(std::uint64_t ieee, const char* key, bool query, std::uint8_t ep,
                 std::uint16_t cluster, const std::uint8_t* bytes, std::size_t len,
                 std::uint32_t now_ms) {
    if (!key || len > kFrameMax) return false;
    if (!query && (!bytes || len == 0)) return false;

    Frame* slot = nullptr;
    for (auto& e : slots_) {
        if (e.used && e.ieee == ieee && std::strncmp(e.key, key, kKeyMax) == 0) { slot = &e; break; }
    }
    if (!slot) {
        for (auto& e : slots_) {
            if (!e.used) { slot = &e; break; }
        }
    }
    if (!slot) {                                        // full: the oldest goes
        slot = &slots_[0];
        for (auto& e : slots_) {
            if (now_ms - e.born_ms > now_ms - slot->born_ms) slot = &e;
        }
    }
    *slot = Frame{};
    slot->used    = true;
    slot->query   = query;
    slot->ieee    = ieee;
    std::strncpy(slot->key, key, kKeyMax);
    slot->key[kKeyMax] = '\0';
    slot->ep      = ep;
    slot->cluster = cluster;
    slot->len     = static_cast<std::uint8_t>(len);
    if (len) std::memcpy(slot->bytes, bytes, len);
    slot->born_ms = now_ms;
    slot->last_ms = now_ms;                             // the first copy went out just now
    return true;
}

bool Queue::take_due(std::uint32_t now_ms, Frame& out) {
    for (auto& e : slots_) {
        if (!e.used || !e.due) continue;
        e.due     = false;
        e.tries  += 1;
        e.last_ms = now_ms;
        out = e;
        return true;
    }
    return false;
}

std::size_t Queue::pending(std::uint64_t ieee) const {
    std::size_t n = 0;
    for (const auto& e : slots_) {
        if (e.used && e.ieee == ieee) ++n;
    }
    return n;
}

}  // namespace zhac_wake
