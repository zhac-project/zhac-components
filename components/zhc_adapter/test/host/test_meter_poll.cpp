// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Meter polling policy (src/meter_poll.hpp): which devices are read, which
// attributes, how often, staggered, capped, and when polling stops. Then the
// availability pings: who is pinged, when, and how they back off.
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

// ── Availability pings (z2m availability.active) ─────────────────────

static const std::uint32_t kPing = 10u * kMin;   // z2m availability.active.timeout

// Take the pings due at `now` (one per tick, as meter_tick does) and ack them as sent.
static std::size_t ping(Scheduler& s, std::uint32_t now, Due* out = nullptr) {
    Due tmp[1];
    Due* d = out ? out : tmp;
    const std::size_t n = s.take_ping_due(now, d, 1);
    for (std::size_t i = 0; i < n; ++i) s.pinged(d[i].ieee, now, kPing);
    return n;
}

static void add_awake(Scheduler& s, std::uint64_t ieee, std::uint32_t now = 0, std::uint8_t ep = 1) {
    assert(s.add(ieee, 0x1234, 0, now, kMin, true, ep, kPing));
}

static void test_quiet_device_is_pinged() {
    Scheduler s;
    add_awake(s, kPlug, 0, 11);
    const std::uint32_t at = kPing + stagger(kPlug, kPingJitterMs);
    assert(ping(s, at - 1) == 0);
    Due d[1];
    assert(ping(s, at, d) == 1);
    assert(d[0].ieee == kPlug && d[0].nwk == 0x1234 && d[0].ep == 11);
    assert(poll(s, at) == 0);                      // no meter read: its def has no meter_poll
}

static void test_heard_device_is_not_pinged() {
    Scheduler s;
    add_awake(s, kPlug);
    const std::uint32_t j = stagger(kPlug, kPingJitterMs);
    for (std::uint32_t t = 0; t < 5 * kPing; t += kMin) {   // reports every minute
        s.heard(kPlug, t, kPing);
        assert(ping(s, t) == 0);
    }
    const std::uint32_t last = 5 * kPing - kMin;
    assert(ping(s, last + kPing + j - 1) == 0);
    assert(ping(s, last + kPing + j) == 1);        // quiet since the last report
}

static void test_sleepy_device_is_never_pinged() {
    Scheduler s;
    assert(s.add(kPlug, 1, 0, 0, kMin, false, 1, kPing));
    assert(s.add(kPlug2, 2, 0, 0, kMin, true, 1, 0));     // pings off (interval 0)
    for (std::uint32_t t = 0; t < 48u * 60u * kMin; t += kMin) assert(ping(s, t) == 0);
}

// z2m's backoff: an unanswered ping is tried again after 1.5x the timeout,
// then twice as long each time (10, 15, 30, 60, 120 min ...), up to a day.
static void test_unanswered_pings_back_off() {
    Scheduler s;
    add_awake(s, kPlug);
    const std::uint32_t j = stagger(kPlug, kPingJitterMs);
    std::uint32_t t = kPing + j;
    assert(ping(s, t) == 1);
    const std::uint32_t gaps_min[] = {15, 30, 60, 120, 240, 480, 960, 1440, 1440};
    for (const std::uint32_t gap : gaps_min) {
        const std::uint32_t next = t + gap * kMin + j;
        assert(ping(s, next - 1) == 0);
        assert(ping(s, next) == 1);
        t = next;
    }
}

static void test_answer_resets_the_backoff() {
    Scheduler s;
    add_awake(s, kPlug);
    const std::uint32_t j = stagger(kPlug, kPingJitterMs);
    std::uint32_t t = kPing + j;
    assert(ping(s, t) == 1);                       // unanswered
    t += 15 * kMin + j;
    assert(ping(s, t) == 1);                       // this one is answered:
    s.heard(kPlug, t + 2000, kPing);
    assert(ping(s, t + 2000 + kPing + j - 1) == 0);
    assert(ping(s, t + 2000 + kPing + j) == 1);    // back to the plain timeout
}

// pinged() says true when the ping before went unanswered, once per silence.
static void test_unanswered_ping_warns_once() {
    Scheduler s;
    add_awake(s, kPlug);
    assert(!s.pinged(kPlug, 0, kPing));            // first ping: nothing missed yet
    assert(s.pinged(kPlug, 0, kPing));             // the one before got no answer
    assert(!s.pinged(kPlug, 0, kPing));            // still silent: said already
    s.heard(kPlug, 0, kPing);
    assert(!s.pinged(kPlug, 0, kPing));
    assert(s.pinged(kPlug, 0, kPing));             // a new silence warns again
}

// heard() says true for the frame that ends a silence of a ping interval or
// more (in practice the answer to a ping), so the hub can tell the cloud the
// device's last_seen moved although no state changed. Once per silence.
static void test_heard_reports_the_end_of_a_silence() {
    Scheduler s;
    add_awake(s, kPlug);
    assert(!s.heard(kPlug, kMin, kPing));                 // talking: nothing to report
    assert(!s.heard(kPlug, 2 * kMin, kPing));
    const std::uint32_t due = 2 * kMin + kPing + stagger(kPlug, kPingJitterMs);
    assert(!s.heard(kPlug, due - 1, kPing));             // just under the interval
    assert(ping(s, due - 1 + kPing + kPingJitterMs) == 1);  // silent: pinged
    assert(s.heard(kPlug, due + kPing + kPingJitterMs, kPing));  // the answer ends the silence
    assert(!s.heard(kPlug, due + kPing + kPingJitterMs + 1000, kPing));
    // Quiet past the due time but not pinged yet (the queue was busy): still a silence.
    s.add(kPlug2, 2, 0, 0, kMin, true, 1, kPing);
    assert(s.heard(kPlug2, kPing + kPingJitterMs, kPing));
    assert(!s.heard(0xDEADull, kPing, kPing));           // unknown device
}

static void test_pings_go_one_per_tick() {
    Scheduler s;
    for (std::uint64_t i = 1; i <= 5; ++i) add_awake(s, i);
    std::size_t total = 0;
    for (std::uint32_t t = kPing + kPingJitterMs; t < kPing + kPingJitterMs + 5 * kTickMs; t += kTickMs) {
        total += ping(s, t);
    }
    assert(total == 5);                            // none lost, none twice
}

static void test_ping_wraparound() {
    Scheduler s;
    const std::uint32_t now = 0xFFFFFFFFu - 1000u;
    add_awake(s, kPlug, now);
    assert(ping(s, now + kPing + stagger(kPlug, kPingJitterMs)) == 1);
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
    test_quiet_device_is_pinged();
    test_heard_device_is_not_pinged();
    test_sleepy_device_is_never_pinged();
    test_unanswered_pings_back_off();
    test_answer_resets_the_backoff();
    test_unanswered_ping_warns_once();
    test_heard_reports_the_end_of_a_silence();
    test_pings_go_one_per_tick();
    test_ping_wraparound();
    std::puts("meter_poll: all passed");
    return 0;
}
