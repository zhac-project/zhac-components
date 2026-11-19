// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The availability ping's glue in zhc_adapter.cpp, cut out at build time (see CMakeLists.txt) and run against the
// real scheduler (meter_poll.cpp): what counts as an answer, and when a ping is booked (research 2026-11-18 B7,
// review 2026-11-19 P3 #3 and #4).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "meter_poll.hpp"
#include "zhc_adapter.h"

template <class... A> static void log_sink(const char*, A&&...) {}
#define ESP_LOGW(tag, ...) log_sink(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) log_sink(tag, __VA_ARGS__)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
using SemaphoreHandle_t = int*;
static int               g_mtx_storage;
static SemaphoreHandle_t g_meter_mtx = &g_mtx_storage;
static int  xSemaphoreTake(SemaphoreHandle_t, std::uint32_t) { return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) {}
static const char* TAG = "zhc_adapter";

constexpr std::uint32_t kPingIntervalMs = 10u * 60u * 1000u;
static std::uint32_t g_now = 0;
static std::uint32_t wake_now_ms() { return g_now; }
static zhac_meter::Scheduler    g_meter;
static zhac_seen_fn_t           g_seen_cb  = nullptr;
static zhac_configure_read_fn_t g_cfg_read = nullptr;

// What the definition checks make of a frame from a device the hub has no definition for: nothing.
static int  g_decoded = 0;
static bool decode_frame(uint64_t, const char*, const char*, uint16_t, uint16_t, uint8_t, uint8_t, const uint8_t*,
                         size_t) {
    g_decoded++;
    return false;
}
void ping_heard(uint64_t ieee);

#include "ping_glue.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

constexpr std::uint64_t kPlug = 0x00158D0000000001ULL;
static const std::uint8_t kReadRsp[] = {0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x20, 0x08};   // zclVersion 8

static void frame_from(uint64_t ieee) {
    zhac_adapter_try_decode(ieee, nullptr, nullptr, 0, 0x0000, 1, 200, kReadRsp, sizeof(kReadRsp));
}

// The radio, for the ping.
static int  g_sent = 0;
static bool nobody_answers(uint64_t, uint16_t, uint8_t, uint16_t, const uint8_t*, uint8_t, uint16_t) {
    g_sent++;
    return true;
}
static bool not_sent(uint64_t, uint16_t, uint8_t, uint16_t, const uint8_t*, uint8_t, uint16_t) {
    g_sent++;
    return false;   // the stack's buffers were full
}
// The answer is handled before the sending task books the ping (it was preempted in between).
static bool answers_at_once(uint64_t ieee, uint16_t, uint8_t, uint16_t, const uint8_t*, uint8_t, uint16_t) {
    g_sent++;
    frame_from(ieee);
    return true;
}

static std::uint64_t g_seen = 0;
static void seen(uint64_t ieee) { g_seen = ieee; }

// A mains device, quiet long enough to be pinged; the clock at its ping.
static std::uint32_t pinged_with(zhac_configure_read_fn_t radio) {
    g_meter.forget(kPlug);
    g_meter.add(kPlug, 0x1234, 0, 0, 60u * 1000u, true, 1, kPingIntervalMs);
    g_now = kPingIntervalMs + zhac_meter::kPingJitterMs;
    g_cfg_read = radio;
    g_sent = 0;
    ping_quiet_device(g_now);
    return g_now;
}

static std::size_t offline_after_wait(std::uint32_t ping_at) {
    std::uint64_t out[4];
    return g_meter.take_offline(ping_at + zhac_meter::kOfflineMs, out, 4);
}

int main() {
    g_seen_cb = seen;

    std::uint32_t at = pinged_with(nobody_answers);
    expect("a quiet mains device is pinged", g_sent == 1);
    expect("  and goes offline when nothing answers", offline_after_wait(at) == 1);

    // Review P3 #3: a device the hub has no definition for (or a fallback one after a reboot) still answers.
    at = pinged_with(nobody_answers);
    g_now += 1000;
    frame_from(kPlug);
    expect("a frame the definitions cannot decode is still an answer", offline_after_wait(at) == 0);

    at = pinged_with(nobody_answers);
    offline_after_wait(at);
    g_seen = 0;
    g_now = at + zhac_meter::kOfflineMs + 1000;
    frame_from(kPlug);
    expect("  and brings an offline device back", g_seen == kPlug && !g_meter.offline(kPlug));

    // Review P3 #4: the ping is booked before it goes out, and a ping the radio did not send is the hub's miss.
    at = pinged_with(answers_at_once);
    expect("an answer handled before the ping is booked still counts", offline_after_wait(at) == 0);
    at = pinged_with(not_sent);
    expect("a ping the radio did not send does not count against the device", g_sent == 1 && offline_after_wait(at) == 0);

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
