// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Send-when-awake queue for devices that sleep between polls.
//
// A sleeping end device only hears its parent right after it polls. A frame
// for it waits in the parent's indirect queue for ~7.7 s and is then dropped
// without a word, so a write or a Tuya query sent while a battery radiator
// valve dozed simply never arrived (Saswell SEA801: Configure, schedule saves).
// The adapter keeps the idempotent frames it sent to such a device and sends
// them again right after the device next transmits -- it is awake then.
//
// A held write is dropped as soon as the device reports its key (applied, or
// changed on the device since: a newer value must not be overwritten), after
// kMaxTries resends, or after kMaxAgeMs. A held query has no key that proves
// it arrived and is resent once. Pure C++, no ESP-IDF, so the policy has a
// host test (test/host); the adapter adds the lock, the task and the radio.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zhac_wake {

constexpr std::size_t   kSlots    = 16;
constexpr std::size_t   kKeyMax   = 28;                    // ATTR_KEY_MAX
constexpr std::size_t   kFrameMax = 64;                    // the adapter's encode buffer
constexpr std::uint32_t kMaxAgeMs = 60u * 60u * 1000u;     // an hour
constexpr std::uint32_t kMinGapMs = 10u * 1000u;           // past the ~7.7 s indirect hold
constexpr std::uint8_t  kMaxTries = 3;

struct Frame {
    bool          used;
    bool          query;       // Tuya dataQuery: resent once, no key to confirm it
    bool          due;         // marked by on_wake, collected by take_due
    std::uint64_t ieee;
    std::uint16_t nwk;         // the address the device last spoke from
    char          key[kKeyMax + 1];
    std::uint8_t  ep;
    std::uint16_t cluster;
    std::uint8_t  len;
    std::uint8_t  bytes[kFrameMax];
    std::uint32_t born_ms;
    std::uint32_t last_ms;     // last (re)send
    std::uint8_t  tries;       // resends so far
};

// Safe to send twice: Tuya setData (0x00 / 0x04) and dataQuery (0x03) on
// 0xEF00, and ZCL global Write Attributes (0x02). Commands such as a toggle
// or a scene recall are not held -- a second copy would act twice.
bool holdable(std::uint16_t cluster, const std::uint8_t* zcl, std::size_t len);

class Queue {
public:
    // Keep a frame. Replaces the entry with the same (ieee, key), else takes a
    // free slot, else the oldest. False if it does not fit.
    bool hold(std::uint64_t ieee, const char* key, bool query, std::uint8_t ep,
              std::uint16_t cluster, const std::uint8_t* bytes, std::size_t len,
              std::uint32_t now_ms);

    // `ieee` just transmitted from `nwk`. Drops its entries that expired, whose
    // key it reported (reported(key) true) or that are out of tries; marks the
    // rest due for a resend, at most once per kMinGapMs. Returns how many are due.
    template <typename Reported>
    std::size_t on_wake(std::uint64_t ieee, std::uint16_t nwk, std::uint32_t now_ms,
                        Reported reported) {
        std::size_t due = 0;
        for (auto& e : slots_) {
            if (!e.used || e.ieee != ieee) continue;
            if (now_ms - e.born_ms > kMaxAgeMs) { e = Frame{}; continue; }
            if (!e.query && reported(static_cast<const char*>(e.key))) { e = Frame{}; continue; }
            if (e.due) { e.nwk = nwk; ++due; continue; }
            if (now_ms - e.last_ms < kMinGapMs) continue;
            if (e.tries >= (e.query ? 1 : kMaxTries)) { e = Frame{}; continue; }
            e.due = true;
            e.nwk = nwk;
            ++due;
        }
        return due;
    }

    // Copy one due frame into `out` and record its resend. False when none is due.
    bool take_due(std::uint32_t now_ms, Frame& out);

    std::size_t pending(std::uint64_t ieee) const;

private:
    Frame slots_[kSlots] = {};
};

}  // namespace zhac_wake
