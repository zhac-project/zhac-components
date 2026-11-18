// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_payload: an MQTT payload longer than the event's field reaches a consumer whole while it is one of the
// last 16 long ones, else as its first 27 characters (research 2026-11-18 A12).
#include "mqtt_payload.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static MqttMsgEvent msg(const std::string& p) {
    MqttMsgEvent m{};
    mqtt_msg_set_payload(m, p.data(), p.size());
    return m;
}
static std::string full(const MqttMsgEvent& m, size_t cap = kMqttPayloadMax) {
    static char out[kMqttPayloadMax];
    return std::string(out, mqtt_msg_full_payload(m, out, cap < sizeof(out) ? cap : sizeof(out)));
}

int main() {
    MqttMsgEvent m = msg("on");
    expect("short: in the event", std::strcmp(m.payload, "on") == 0 && full(m).empty());
    const std::string p31(31, 'a');
    m = msg(p31);
    expect("31 bytes: in the event, whole", std::string(m.payload, strnlen(m.payload, 32)) == p31 && full(m).empty());
    m = msg("");
    expect("empty: in the event", m.payload[0] == '\0' && full(m).empty());

    const std::string json = R"({"temperature":21.5,"humidity":48.2,"battery":90})";
    m = msg(json);
    expect("long: the event has its first 27 characters", std::string(m.payload) == json.substr(0, 27));
    expect("long: the whole text by its id", full(m) == json);
    expect("long: not into a smaller buffer", full(m, json.size() - 1).empty());

    std::string big(3000, 'x');
    big[1023] = 'y';
    m = msg(big);
    expect("past 1 KB: its first 1 KB", full(m) == big.substr(0, kMqttPayloadMax));

    MqttMsgEvent first = msg(json + "#0");
    MqttMsgEvent later[16];
    for (int i = 0; i < 16; i++) later[i] = msg(json + "#" + std::to_string(i + 1));
    expect("16 newer long ones: the oldest is gone, its 27 characters stay",
           full(first).empty() && std::string(first.payload) == json.substr(0, 27));
    expect("  the 16 newer ones are whole", full(later[0]) == json + "#1" && full(later[15]) == json + "#16");

    std::string bin(31, 'b');
    bin[27] = '\0';
    std::memcpy(&bin[28], "\x01\x00\x00", 3);   // a NUL at 27 and bytes that could read as an id
    m = msg(bin);
    expect("a short binary payload is never read as an id", full(m).empty());

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail;
}
