// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_bridge_on_mqtt_rx(), cut out of ha_bridge.cpp at build time (CMakeLists.txt): a `/set` command
// reaches the device whether Home Assistant discovery is on or off (research 2026-11-18 A12).
// The queue and the root topic are fakes; the topic and value parsing are the real ha_discovery.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ha_discovery.h"

#define ESP_LOGW(tag, ...) ((void)(tag))
static const char* TAG = "ha_bridge";
enum class Op : uint8_t { RepublishAll, Device, Removed, Command, RetractAll };
static bool        s_enabled = false;
static int         g_posted  = 0;
static std::string g_key, g_value;
static uint64_t    g_ieee = 0;
static bool post(Op op, uint64_t ieee = 0, const char* key = nullptr, const char* value = nullptr) {
    if (op != Op::Command) return false;
    g_posted++;
    g_ieee  = ieee;
    g_key   = key ? key : "";
    g_value = value ? value : "";
    return true;
}
static const char* mqtt_gw_get_root_topic() { return "zhac"; }

#include "on_mqtt_rx.inc"

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}
static bool rx(const char* topic, const char* data) {
    g_posted = 0;
    g_ieee   = 0;
    g_key.clear();
    return ha_bridge_on_mqtt_rx(topic, (int)std::strlen(topic), data, (int)std::strlen(data));
}

int main() {
    const char* set = "zhac/devices/00158D0007A1B2C3/state/set";
    s_enabled = true;
    expect("discovery on: a /set command is taken", rx(set, "ON") && g_posted == 1);
    expect("a state topic is not a command", !rx("zhac/devices/00158D0007A1B2C3/state", "ON") && g_posted == 0);
    expect("another root's command is not ours", !rx("other/devices/00158D0007A1B2C3/state/set", "ON") && g_posted == 0);
    s_enabled = false;
    expect("discovery off: a /set command is taken too", rx(set, "ON"));
    expect("  and queued for that device and key", g_posted == 1 && g_ieee == 0x00158D0007A1B2C3ULL && g_key == "state");
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
