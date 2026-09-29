// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// The hub's answers to a Tuya MCU's own requests on 0xEF00 (src/tuya_mcu.hpp):
// fed the frames the device sends, checked byte for byte against what z2m's
// tuyaBase sends back (zigbee-herdsman-converters v26.105.0 lib/tuya.ts).
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "tuya_mcu.hpp"

// Device -> hub (herdsman manuSpecificTuya commandsResponse): frame control
// 0x09 = cluster-specific, server to client; TSN; command; payloadSize u16 LE.
static const std::uint8_t kSyncTimeReq[]      = {0x09, 0x05, 0x24, 0x08, 0x00};  // mcuSyncTime
static const std::uint8_t kGatewayStatusReq[] = {0x09, 0x06, 0x25, 0x01, 0x00};  // mcuGatewayConnectionStatus

static const std::time_t kSummer = 1782907200;   // 2026-07-01 12:00:00 UTC
static const std::time_t kWinter = 1768478400;   // 2026-01-15 12:00:00 UTC
static const char* kKyiv = "EET-2EEST,M3.5.0/3,M10.5.0/4";

static void tz(const char* zone) {
    setenv("TZ", zone, 1);
    tzset();
}

template <std::size_t N>
static bool answers(const std::uint8_t (&req)[N], std::uint8_t time_start, std::time_t now,
                    std::uint8_t cmd, const std::uint8_t* want, std::size_t want_len) {
    std::uint8_t out[10];
    std::size_t n = 99;
    const std::uint8_t got = tuya_mcu::reply(req, N, time_start, now, out, n);
    return got == cmd && n == want_len && (want_len == 0 || std::memcmp(out, want, n) == 0);
}

static void test_gateway_status_is_connected() {
    // z2m: {payloadSize: 1, payload: 1} -> 01 00 01, whatever the time settings.
    const std::uint8_t want[] = {0x01, 0x00, 0x01};
    assert(answers(kGatewayStatusReq, 0, 0, 0x25, want, 3));
    assert(answers(kGatewayStatusReq, 1, kSummer, 0x25, want, 3));
}

static void test_sync_time_utc_and_local() {
    // payloadSize 8 (u16 LE), then UTC and local seconds, big-endian.
    tz(kKyiv);   // EEST, UTC+3
    const std::uint8_t s1970[] = {0x08, 0x00, 0x6A, 0x45, 0x01, 0x40, 0x6A, 0x45, 0x2B, 0x70};
    assert(answers(kSyncTimeReq, 1, kSummer, 0x24, s1970, 10));
    const std::uint8_t s2000[] = {0x08, 0x00, 0x31, 0xD7, 0xBD, 0xC0, 0x31, 0xD7, 0xE7, 0xF0};
    assert(answers(kSyncTimeReq, 2, kSummer, 0x24, s2000, 10));
    const std::uint8_t w1970[] = {0x08, 0x00, 0x69, 0x68, 0xD6, 0xC0, 0x69, 0x68, 0xF2, 0xE0};
    assert(answers(kSyncTimeReq, 1, kWinter, 0x24, w1970, 10));   // EET, UTC+2
    tz("UTC0");  // no timezone set: local == UTC
    const std::uint8_t utc[] = {0x08, 0x00, 0x69, 0x68, 0xD6, 0xC0, 0x69, 0x68, 0xD6, 0xC0};
    assert(answers(kSyncTimeReq, 1, kWinter, 0x24, utc, 10));
}

static void test_sync_time_not_answered() {
    assert(answers(kSyncTimeReq, 0, kSummer, 0, nullptr, 0));       // z2m timeStart "off"
    assert(answers(kSyncTimeReq, 1, 1000, 0, nullptr, 0));          // our clock not set yet
}

static void test_other_frames_ignored() {
    const std::uint8_t report[] = {0x09, 0x07, 0x02, 0x00, 0x10, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0xE6};
    assert(answers(report, 1, kSummer, 0, nullptr, 0));             // dataReport
    const std::uint8_t dflt[] = {0x18, 0x05, 0x0B, 0x25, 0x00};
    assert(answers(dflt, 1, kSummer, 0, nullptr, 0));               // global Default Response
    const std::uint8_t cut[] = {0x09, 0x05};
    assert(answers(cut, 1, kSummer, 0, nullptr, 0));
    // A manufacturer code moves the command id two bytes on.
    const std::uint8_t manu[] = {0x0D, 0x02, 0x10, 0x07, 0x25, 0x01, 0x00};
    const std::uint8_t want[] = {0x01, 0x00, 0x01};
    assert(answers(manu, 0, 0, 0x25, want, 3));
}

int main() {
    test_gateway_status_is_connected();
    test_sync_time_utc_and_local();
    test_sync_time_not_answered();
    test_other_frames_ignored();
    return 0;
}
