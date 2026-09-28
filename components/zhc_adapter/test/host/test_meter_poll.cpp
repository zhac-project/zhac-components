// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Meter polling policy (src/meter_poll.hpp): which devices are read, which
// attributes, how often, staggered, capped, and when polling stops.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "meter_poll.hpp"

using namespace zhac_meter;

static const std::uint64_t kPlug  = 0xA4C1380000000001ull;
static const std::uint64_t kPlug2 = 0xA4C1380000000002ull;
static const std::uint32_t kMin   = 60u * 1000u;
static const std::uint8_t  kBoth  = kElectrical | kMetering;

// Collect everything due at `now` (up to the cap) and ack it as sent.
static std::size_t poll(Scheduler& s, std::uint32_t now, Due* out = nullptr) {
    Due tmp[kMaxPerTick];
    Due* d = out ? out : tmp;
    const std::size_t n = s.take_due(now, d, kMaxPerTick);
    for (std::size_t i = 0; i < n; ++i) s.polled(d[i].ieee, now, kMin);
    return n;
}

static void test_reads_carry_z2m_attrs() {
    Read r[2];
    assert(reads_for(kElectrical, r) == 1);
    assert(r[0].cluster == 0x0B04 && r[0].count == 3);
    const std::uint8_t em[] = {0x05, 0x05, 0x08, 0x05, 0x0B, 0x05};  // 0x0505 0x0508 0x050B LE
    for (int i = 0; i < 6; ++i) assert(r[0].attrs_le[i] == em[i]);

    assert(reads_for(kBoth, r) == 2);
    assert(r[1].cluster == 0x0702 && r[1].count == 1);
    assert(r[1].attrs_le[0] == 0x00 && r[1].attrs_le[1] == 0x00);    // currentSummDelivered

    assert(reads_for(kMetering, r) == 1 && r[0].cluster == 0x0702);
    assert(reads_for(0, r) == 0);
}

static void test_mains_only() {
    assert(mains_powered(0x01) && mains_powered(0x02) && mains_powered(0x04));
    assert(mains_powered(0x05) && mains_powered(0x06));
    assert(mains_powered(0x81));             // mains with backup battery
    assert(!mains_powered(0x03) && !mains_powered(0x83));   // battery
    assert(!mains_powered(0x00));            // unknown: caller decides
}

static void test_polls_every_interval() {
    Scheduler s;
    s.add(kPlug, 0x1234, kBoth, 0, kMin);
    const std::uint32_t first = stagger(kPlug, kMin);
    assert(first < kMin);
    if (first > 0) assert(poll(s, first - 1) == 0);
    Due d[kMaxPerTick];
    assert(poll(s, first, d) == 1);
    assert(d[0].ieee == kPlug && d[0].nwk == 0x1234 && d[0].flags == kBoth);
    assert(poll(s, first + 1) == 0);
    assert(poll(s, first + kMin - 1) == 0);
    assert(poll(s, first + kMin) == 1);
    assert(poll(s, first + 2 * kMin) == 1);
}

// I-2: a failed read backs off to the full interval, same as a successful
// one, instead of retrying every kTickMs. The caller (zhc_adapter's
// meter_tick) always calls polled(..., ok) whether the send succeeded or
// not -- this pins that behaviour at the Scheduler level.
static void test_failed_poll_backs_off_to_interval() {
    Scheduler s;
    s.add(kPlug, 1, kBoth, 0, kMin);
    Due d[kMaxPerTick];
    const std::uint32_t t = stagger(kPlug, kMin);
    assert(s.take_due(t, d, kMaxPerTick) == 1);
    assert(s.polled(kPlug, t, kMin, false) == true);      // first failure: log once
    assert(s.take_due(t + kTickMs, d, kMaxPerTick) == 0);  // no retry next 5 s tick
    assert(s.take_due(t + kMin - 1, d, kMaxPerTick) == 0);
    assert(s.take_due(t + kMin, d, kMaxPerTick) == 1);     // retried after the interval
}

// Warn once per failure streak; a success resets it so the next failure
// warns again.
static void test_failure_streak_warns_once() {
    Scheduler s;
    s.add(kPlug, 1, kBoth, 0, kMin);
    std::uint32_t t = stagger(kPlug, kMin);
    assert(s.polled(kPlug, t, kMin, false) == true);    // 1st failure in streak: warn
    assert(s.polled(kPlug, t, kMin, false) == false);   // still in streak: silent
    assert(s.polled(kPlug, t, kMin, false) == false);   // still in streak: silent
    assert(s.polled(kPlug, t, kMin, true)  == false);   // success resets the streak
    assert(s.polled(kPlug, t, kMin, false) == true);    // new streak: warn again
}

static void test_stagger_spreads_devices() {
    // Distinct devices land on distinct offsets inside one interval.
    int same = 0;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        for (std::uint64_t j = i + 1; j <= 20; ++j) {
            if (stagger(0xA4C1380000000000ull + i, kMin) == stagger(0xA4C1380000000000ull + j, kMin)) ++same;
        }
    }
    assert(same == 0);
    assert(stagger(kPlug, kMin) != stagger(kPlug2, kMin));
}

static void test_flagless_or_battery_never_polled() {
    Scheduler s;
    s.add(kPlug, 1, 0, 0, kMin);                   // no meter_poll in its def
    assert(s.known(kPlug));                        // tracked, so no re-lookup
    for (std::uint32_t t = 0; t < 3 * kMin; t += kTickMs) assert(poll(s, t) == 0);
}

static void test_removed_device_stops() {
    Scheduler s;
    s.add(kPlug, 1, kBoth, 0, kMin);
    s.add(kPlug2, 2, kBoth, 0, kMin);
    s.forget(kPlug);
    assert(!s.known(kPlug));
    Due d[kMaxPerTick];
    assert(poll(s, 2 * kMin, d) == 1 && d[0].ieee == kPlug2);
    assert(poll(s, 4 * kMin, d) == 1 && d[0].ieee == kPlug2);
    s.forget(kPlug2);
    assert(poll(s, 6 * kMin) == 0);
}

static void test_scan_sweeps_devices_gone_from_pool() {
    Scheduler s;
    s.add(kPlug, 1, kBoth, 0, kMin);
    s.add(kPlug2, 2, kBoth, 0, kMin);
    s.begin_scan();
    assert(s.refresh(kPlug2, 0x9999));             // still in the pool, new nwk
    assert(!s.refresh(0xDEADull, 1));              // never added
    s.end_scan();
    assert(!s.known(kPlug) && s.known(kPlug2));
    Due d[kMaxPerTick];
    assert(poll(s, kMin, d) == 1 && d[0].ieee == kPlug2 && d[0].nwk == 0x9999);
}

static void test_in_flight_cap() {
    Scheduler s;
    for (std::uint64_t i = 1; i <= 10; ++i) s.add(i, static_cast<std::uint16_t>(i), kBoth, 0, kMin);
    // All due at once (well past every stagger): at most kMaxPerTick per tick,
    // the rest wait for the next ticks, none is lost.
    std::uint32_t t = kMin;
    std::size_t total = 0;
    for (int tick = 0; tick < 10; ++tick, t += kTickMs) {
        const std::size_t n = poll(s, t);
        assert(n <= kMaxPerTick);
        total += n;
    }
    assert(total == 10);
}

static void test_table_full() {
    Scheduler s;
    for (std::uint64_t i = 1; i <= kSlots; ++i) assert(s.add(i, 1, kBoth, 0, kMin));
    assert(!s.add(kSlots + 1, 1, kBoth, 0, kMin));
    assert(s.add(1, 7, kBoth, 0, kMin));           // re-add of a known one is fine
}

static void test_wraparound() {
    Scheduler s;
    const std::uint32_t now = 0xFFFFFFFFu - 1000u;
    s.add(kPlug, 1, kBoth, now, kMin);
    assert(poll(s, now + stagger(kPlug, kMin)) == 1);   // crosses 2^32 fine
}

int main() {
    test_reads_carry_z2m_attrs();
    test_mains_only();
    test_polls_every_interval();
    test_failed_poll_backs_off_to_interval();
    test_failure_streak_warns_once();
    test_stagger_spreads_devices();
    test_flagless_or_battery_never_polled();
    test_removed_device_stops();
    test_scan_sweeps_devices_gone_from_pool();
    test_in_flight_cap();
    test_table_full();
    test_wraparound();
    std::puts("meter_poll: all passed");
    return 0;
}
