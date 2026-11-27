// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// zhc_adapter.cpp's glue, cut out at build time (see CMakeLists.txt): how a decoded report reaches the shadow
// bridge (review 2026-11-19 P3 #1), and, against the real scheduler (meter_poll.cpp), what answers the
// availability ping and when a ping is booked (research 2026-11-18 B7, review P3 #3 and #4), and what a send nobody
// took does to it (B1).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "meter_poll.hpp"
#include "zhc_adapter.h"
#include "zhc/runtime/dispatch.hpp"

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
static zhac_shadow_update_fn_t  g_shadow_update = nullptr;

// What the definition checks make of a frame from a device the hub has no definition for: nothing.
static int  g_decoded = 0;
static bool decode_frame(uint64_t, const char*, const char*, uint16_t, uint16_t, uint8_t, uint8_t, const uint8_t*,
                         size_t) {
    g_decoded++;
    return false;
}
void ping_heard(uint64_t ieee);

#include "adapter_glue.inc"

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

// The shadow bridge, for fire_shadow_updates.
struct ShadowCall { std::uint64_t ieee; std::string key; std::uint8_t kind; };
static std::vector<ShadowCall> g_shadow_calls;
static void shadow_bridge(uint64_t ieee, const char* key, uint8_t kind, int64_t, uint64_t, float, bool, const char*) {
    g_shadow_calls.push_back({ieee, key ? key : "", kind});
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

// Long enough for an offline verdict: the ping's answer window, and an hour of silence.
static std::size_t offline_after_wait(std::uint32_t ping_at) {
    std::uint64_t out[4];
    return g_meter.take_offline(ping_at + zhac_meter::kActiveOfflineMs, out, 4);
}

int main() {
    g_seen_cb = seen;

    // Review P3 #1: the bridge hands a report's keys to the shadow in one call at the report's end.
    g_shadow_update = shadow_bridge;
    zhc::DispatchResult r{};
    zhc::Value v{};
    v.type = zhc::ValueType::Uint;
    v.u = 230;
    r.merged.put("voltage", v);
    v.u = 115;
    r.merged.put("power", v);
    fire_shadow_updates(kPlug, r);
    expect("a report's keys go to the shadow bridge one per call, then the report's end",
           g_shadow_calls.size() == 3 && g_shadow_calls[0].key == "voltage" && g_shadow_calls[1].key == "power" &&
               g_shadow_calls[2].kind == ZHAC_SHADOW_KIND_REPORT_END && g_shadow_calls[2].ieee == kPlug);

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
    g_now = at + zhac_meter::kActiveOfflineMs + 1000;
    frame_from(kPlug);
    expect("  and brings an offline device back", g_seen == kPlug && !g_meter.offline(kPlug));

    // Review P3 #4: the ping is booked before it goes out, and a ping the radio did not send is the hub's miss.
    at = pinged_with(answers_at_once);
    expect("an answer handled before the ping is booked still counts", offline_after_wait(at) == 0);
    at = pinged_with(not_sent);
    expect("a ping the radio did not send does not count against the device", g_sent == 1 && offline_after_wait(at) == 0);

    // Research B1: a frame nobody took, not even after the hub looked the address up again, and no answer to the lookup.
    g_meter.forget(kPlug);
    g_meter.add(kPlug, 0x1234, 0, 0, 60u * 1000u, true, 1, kPingIntervalMs);
    g_now = 5000;
    zhac_adapter_unreachable(kPlug);
    std::uint64_t gone[4];
    expect("a device nothing reaches, not even its address lookup, counts as an unanswered ping",
           g_meter.take_offline(g_now, gone, 4) == 0);
    g_now = zhac_meter::kActiveOfflineMs;
    expect("  and is offline once it has been silent for the hour",
           g_meter.take_offline(g_now, gone, 4) == 1 && gone[0] == kPlug);

    // Owner 2026-11-27: a battery device, never pinged, is offline after 48 h of silence; its next frame is reported.
    g_meter.forget(kPlug);
    g_meter.add(kPlug, 0x1234, 0, 0, 60u * 1000u, false, 1, kPingIntervalMs);
    g_now = zhac_meter::kPassiveOfflineMs;
    expect("a battery device silent for 48 h is offline", g_meter.take_offline(g_now, gone, 4) == 1 && gone[0] == kPlug);
    g_seen = 0;
    g_now += 1000;
    frame_from(kPlug);
    expect("  and its next frame brings it back", g_seen == kPlug && !g_meter.offline(kPlug));

    // Review 2026-11-27: a Device Announce (a light switched back on at the wall), which never reaches the decoder.
    at = pinged_with(nobody_answers);
    offline_after_wait(at);
    g_seen = 0;
    zhac_adapter_heard(kPlug);
    expect("a Device Announce brings an offline device back", g_seen == kPlug && !g_meter.offline(kPlug));

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
