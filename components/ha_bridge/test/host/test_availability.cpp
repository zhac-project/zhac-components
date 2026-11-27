// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The device availability topic's state machine, cut out of ha_bridge.cpp (see CMakeLists.txt): battery devices go
// offline after 48 h of silence, mains devices when the availability ping goes unanswered (research 2026-11-18 B7),
// and the tick repairs what an event or a publish lost (review 2026-11-19 P3 #2).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ha_bridge.h"

#define ESP_LOGI(tag, ...) ((void)(tag))
static const char* TAG = "ha_bridge";
constexpr uint32_t kPassiveSilenceS = 48 * 3600;
static uint32_t    g_now = 1000;
static uint32_t    now_s() { return g_now; }
static std::string g_pub;   // what reached the broker, "<ieee low byte>=online;..."
static bool        g_broker_ok = true;   // false: the publish gives up (queue full for 2 s)
static int         g_attempts = 0;
static bool publish_availability(uint64_t ieee, bool online) {
    g_attempts++;
    if (!g_broker_ok) return false;
    char b[32];
    std::snprintf(b, sizeof b, "%02X=%s;", (unsigned)(ieee & 0xFF), online ? "online" : "offline");
    g_pub += b;
    return true;
}
struct Published;
static Published* slot_for(uint64_t ieee, bool create);

// For tick_availability.
constexpr size_t kMaxDevices = 2;
static bool      s_enabled = true;
static Published* s_pub = nullptr;
const HaBridgePlatform* s_plat = nullptr;
static bool g_connected = true;
static bool mqtt_gw_is_connected() { return g_connected; }
static void publish_value(uint64_t, const char*, const char*) {}   // for ha_bridge_publish_state
static uint64_t g_offline_ieee = 0;   // the adapter's verdict: this mains device is offline
static bool device_offline(uint64_t ieee) { return ieee == g_offline_ieee; }
static const HaBridgePlatform kPlat = {"test", nullptr, nullptr, nullptr, device_offline};

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
    expect("a battery device keeps its 48-hour silence rule", took().empty() && !sensor.offline);
    set_online(0xB2, true);
    expect("  an answer restarts its silence clock", took().empty() && sensor.last_seen_s == g_now);

    set_online(0xC3, false);
    expect("a device Home Assistant does not know is ignored", took().empty());

    // Review 2026-11-19 P3 #2: level-triggered. The tick compares each device with the adapter's verdict, so a lost
    // DEVICE_SEEN or DEVICE_OFFLINE, or a publish the broker did not take, is repaired within a second.
    s_pub  = g_slots;
    s_plat = &kPlat;
    tick_availability();
    expect("the tick says nothing while the broker has the adapter's verdict", took().empty());

    g_offline_ieee = 0xA1;
    g_broker_ok = false;
    set_online(0xA1, false);
    g_broker_ok = true;
    expect("a publish the broker did not take reaches nothing", took().empty());
    tick_availability();
    expect("  and the tick sends it again", took() == "A1=offline;");
    tick_availability();
    expect("  once", took().empty());

    g_offline_ieee = 0;   // the device answered; its DEVICE_SEEN was dropped on a full queue
    tick_availability();
    expect("a lost DEVICE_SEEN is repaired by the tick", took() == "A1=online;" && !plug.offline);

    g_offline_ieee = 0xA1;
    g_now = sensor.last_seen_s + kPassiveSilenceS + 1;
    g_broker_ok = false;
    g_attempts = 0;
    tick_availability();
    expect("a broker that takes nothing costs one attempt per tick, not one per device", g_attempts == 1);
    g_broker_ok = true;
    tick_availability();
    expect("  and the next tick sends both", took() == "A1=offline;B2=offline;");

    // Review 2026-11-27: a report while the broker is away still counts, so a long outage does not leave every
    // battery device offline at the reconnect.
    g_connected = false;
    g_now += 1000;
    ha_bridge_publish_state(0xB2, "contact", "true");
    g_connected = true;
    tick_availability();
    expect("a battery device's report while the broker is away restarts its clock",
           took() == "B2=online;" && !sensor.offline && sensor.last_seen_s == g_now);

    std::printf("%s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
