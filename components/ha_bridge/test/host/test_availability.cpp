// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The device availability topic's state machine, cut out of ha_bridge.cpp (see CMakeLists.txt): battery devices go
// offline after a day of silence, mains devices when the availability ping goes unanswered (research 2026-11-18 B7).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define ESP_LOGI(tag, ...) ((void)(tag))
static const char* TAG = "ha_bridge";
constexpr uint32_t kPassiveSilenceS = 25 * 3600;
static uint32_t    g_now = 1000;
static uint32_t    now_s() { return g_now; }
static std::string g_pub;   // what reached the broker, "<ieee low byte>=online;..."
static void publish_availability(uint64_t ieee, bool online) {
    char b[32];
    std::snprintf(b, sizeof b, "%02X=%s;", (unsigned)(ieee & 0xFF), online ? "online" : "offline");
    g_pub += b;
}
struct Published;
static Published* slot_for(uint64_t ieee, bool create);

#include "availability.inc"

static Published g_slots[2];
static Published* slot_for(uint64_t ieee, bool) {
    for (Published& s : g_slots) if (s.ieee == ieee) return &s;
    return nullptr;
}

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static std::string took() {
    std::string s = g_pub;
    g_pub.clear();
    return s;
}

int main() {
    Published& plug = g_slots[0];
    plug.ieee = 0xA1;
    Published& sensor = g_slots[1];
    sensor.ieee = 0xB2;

    sync_availability(plug, false, false);
    expect("a mains device is published online", took() == "A1=online;" && !plug.offline);
    sync_availability(plug, false, true);
    expect("  or offline when its ping went unanswered", took() == "A1=offline;" && plug.offline);
    sync_availability(plug, false, false);
    took();

    sync_availability(sensor, true, true);
    expect("a battery device starts its silence clock, online whatever the ping says",
           took() == "B2=online;" && !sensor.offline && sensor.last_seen_s == g_now);

    // The availability ping's verdict, from DEVICE_OFFLINE / DEVICE_SEEN
    set_online(0xA1, false);
    expect("an unanswered ping takes a mains device offline", took() == "A1=offline;" && plug.offline);
    set_online(0xA1, false);
    expect("  once", took().empty());
    set_online(0xA1, true);
    expect("  and its next answer brings it back", took() == "A1=online;" && !plug.offline);
    set_online(0xA1, true);
    expect("  once", took().empty());

    g_now += 3600;
    set_online(0xB2, false);
    expect("a battery device keeps its day-of-silence rule", took().empty() && !sensor.offline);
    set_online(0xB2, true);
    expect("  an answer restarts its silence clock", took().empty() && sensor.last_seen_s == g_now);

    set_online(0xC3, false);
    expect("a device Home Assistant does not know is ignored", took().empty());

    std::printf("%s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
