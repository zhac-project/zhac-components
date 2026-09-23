// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Send-when-awake policy (src/wake_queue.hpp): what is held, when it is resent,
// and when it is dropped.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "wake_queue.hpp"

using namespace zhac_wake;

static const std::uint64_t kTrv = 0xEC1BBDFFFE2DAD79ull;
static const std::uint64_t kOther = 0x00158D000231E632ull;
// Tuya setData for DP 109 (Saswell schedule write), 27 bytes.
static const std::uint8_t kTuyaSet[27] = {0x01, 0x00, 0x00, 0x00, 0x01, 109, 0x00, 0x00, 18, 0x02, 0x04,
                                           0x01, 0x68, 0x00, 0xC8, 0x01, 0xE0, 0x00, 0x96,
                                           0x03, 0xFC, 0x00, 0xD2, 0x05, 0x28, 0x00, 0x96};
static auto none = [](const char*) { return false; };

static void test_holdable() {
    assert(holdable(0xEF00, kTuyaSet, sizeof(kTuyaSet)));                   // Tuya setData
    const std::uint8_t query[3] = {0x01, 0x00, 0x03};
    assert(holdable(0xEF00, query, 3));                                      // Tuya dataQuery
    const std::uint8_t sync[13] = {0x09, 0x05, 0x24, 0x08, 0x00};
    assert(!holdable(0xEF00, sync, sizeof(sync)));                           // our time answer
    const std::uint8_t write_attr[8] = {0x00, 0x00, 0x02, 0x00, 0x40, 0x30, 0x01, 0x00};
    assert(holdable(0x0201, write_attr, sizeof(write_attr)));                // ZCL Write Attributes
    const std::uint8_t mfg_write[10] = {0x04, 0x5F, 0x11, 0x00, 0x02, 0x00, 0x40, 0x30, 0x01, 0x00};
    assert(holdable(0xFCC0, mfg_write, sizeof(mfg_write)));                  // manufacturer-specific write
    const std::uint8_t toggle[3] = {0x01, 0x00, 0x02};
    assert(!holdable(0x0006, toggle, 3));                                    // On/Off toggle: acts twice
    const std::uint8_t dflt[5] = {0x18, 0x00, 0x0B, 0x02, 0x00};
    assert(!holdable(0x0006, dflt, 5));                                      // default response
    assert(!holdable(0xEF00, nullptr, 0));
}

static void test_report_of_the_key_confirms() {
    Queue q;
    assert(q.hold(kTrv, "schedule_monday", false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000));
    assert(q.pending(kTrv) == 1);
    // The valve reports schedule_monday: applied (or changed on the valve) -- drop it.
    const auto n = q.on_wake(kTrv, 0x12EC, 60000, [](const char* k) { return std::strcmp(k, "schedule_monday") == 0; });
    assert(n == 0 && q.pending(kTrv) == 0);
}

static void test_resend_on_wake_then_give_up() {
    Queue q;
    Frame f{};
    assert(q.hold(kTrv, "schedule_monday", false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000));
    // Talking again inside the gap: the first copy may still sit in the parent's queue.
    assert(q.on_wake(kTrv, 0x12EC, 5000, none) == 0);
    // Next wake-up: due once, with the address it spoke from.
    assert(q.on_wake(kTrv, 0x12EC, 20 * 60 * 1000, none) == 1);
    assert(q.take_due(20 * 60 * 1000, f));
    assert(f.ieee == kTrv && f.nwk == 0x12EC && f.ep == 1 && f.cluster == 0xEF00 && f.tries == 1);
    assert(f.len == sizeof(kTuyaSet) && std::memcmp(f.bytes, kTuyaSet, sizeof(kTuyaSet)) == 0);
    assert(!q.take_due(20 * 60 * 1000, f));                        // one copy per wake-up
    assert(q.on_wake(kTrv, 0x12EC, 20 * 60 * 1000 + 500, none) == 0);   // same burst
    // Two more wake-ups, two more copies, then it gives up.
    assert(q.on_wake(kTrv, 0x12EC, 40 * 60 * 1000, none) == 1 && q.take_due(40 * 60 * 1000, f) && f.tries == 2);
    assert(q.on_wake(kTrv, 0x12EC, 55 * 60 * 1000, none) == 1 && q.take_due(55 * 60 * 1000, f) && f.tries == 3);
    assert(q.on_wake(kTrv, 0x12EC, 58 * 60 * 1000, none) == 0 && q.pending(kTrv) == 0);
}

static void test_expires_after_an_hour() {
    Queue q;
    assert(q.hold(kTrv, "current_heating_setpoint", false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000));
    assert(q.on_wake(kTrv, 0x12EC, 1000 + 61u * 60u * 1000u, none) == 0);
    assert(q.pending(kTrv) == 0);
}

static void test_query_is_resent_once() {
    Queue q;
    Frame f{};
    assert(q.hold(kTrv, "#query", true, 1, 0xEF00, nullptr, 0, 1000));
    // Any report is no proof the dump arrived: a query is not confirmed by keys.
    assert(q.on_wake(kTrv, 0x12EC, 60000, [](const char*) { return true; }) == 1);
    assert(q.take_due(60000, f) && f.query && f.tries == 1);
    assert(q.on_wake(kTrv, 0x12EC, 30 * 60 * 1000, none) == 0 && q.pending(kTrv) == 0);
}

static void test_latest_write_wins_and_devices_are_separate() {
    Queue q;
    std::uint8_t newer[27];
    std::memcpy(newer, kTuyaSet, sizeof(newer));
    newer[14] = 0xD2;
    assert(q.hold(kTrv, "schedule_monday", false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000));
    assert(q.hold(kTrv, "schedule_monday", false, 1, 0xEF00, newer, sizeof(newer), 2000));
    assert(q.hold(kTrv, "schedule_tuesday", false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 2000));
    assert(q.pending(kTrv) == 2);
    assert(q.on_wake(kOther, 0x1111, 60000, none) == 0);           // another device woke
    assert(q.on_wake(kTrv, 0x12EC, 60000, none) == 2);
    Frame f{};
    int monday = 0;
    while (q.take_due(60000, f)) {
        if (std::strcmp(f.key, "schedule_monday") == 0) { ++monday; assert(f.bytes[14] == 0xD2); }
    }
    assert(monday == 1);
}

static void test_full_queue_drops_the_oldest() {
    Queue q;
    char key[16];
    for (unsigned i = 0; i < kSlots + 1; ++i) {
        std::snprintf(key, sizeof(key), "k%u", i);
        assert(q.hold(kTrv, key, false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000 + i));
    }
    assert(q.pending(kTrv) == kSlots);
    bool k0 = false;
    assert(q.on_wake(kTrv, 0x12EC, 60000, [&k0](const char* k) { if (std::strcmp(k, "k0") == 0) k0 = true; return false; }) == kSlots);
    assert(!k0);                                                    // k0 was the oldest
}

static void test_refuses_what_does_not_fit() {
    Queue q;
    std::uint8_t big[kFrameMax + 1] = {0x01, 0x00, 0x00};
    assert(!q.hold(kTrv, "big", false, 1, 0xEF00, big, sizeof(big), 1000));
    assert(!q.hold(kTrv, nullptr, false, 1, 0xEF00, kTuyaSet, sizeof(kTuyaSet), 1000));
    assert(!q.hold(kTrv, "empty", false, 1, 0xEF00, nullptr, 0, 1000));
    assert(q.pending(kTrv) == 0);
}

int main() {
    test_holdable();
    test_report_of_the_key_confirms();
    test_resend_on_wake_then_give_up();
    test_expires_after_an_hour();
    test_query_is_resent_once();
    test_latest_write_wins_and_devices_are_separate();
    test_full_queue_drops_the_oldest();
    test_refuses_what_does_not_fit();
    std::printf("wake_queue host tests: ok\n");
    return 0;
}
