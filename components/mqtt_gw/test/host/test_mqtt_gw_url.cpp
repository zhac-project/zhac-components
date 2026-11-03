// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_gw_url.h: the status shows the broker URL without its user name and password, and a client that
// sends that address back has not changed the broker (review 2026-10-24, LOW round review M6).
#include "mqtt_gw_url.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)

int main() {
    char safe[128];
    mqtt_url_strip_userinfo("mqtt://user:pass@192.168.1.10:1883", safe, sizeof(safe));
    CHECK(std::strcmp(safe, "mqtt://192.168.1.10:1883") == 0, "user:pass@ is stripped for the status");
    mqtt_url_strip_userinfo("mqtts://broker:8883/path@x", safe, sizeof(safe));
    CHECK(std::strcmp(safe, "mqtts://broker:8883/path@x") == 0, "an @ in the path stays");

    const char* stored = "mqtt://user:pass@192.168.1.10:1883";
    CHECK(mqtt_url_is_echo("mqtt://192.168.1.10:1883", stored), "the address the status showed is an echo");
    CHECK(!mqtt_url_is_echo("mqtt://192.168.1.11:1883", stored), "another host is a change");
    CHECK(!mqtt_url_is_echo("mqtt://other:secret@192.168.1.10:1883", stored), "new credentials are a change");
    CHECK(!mqtt_url_is_echo(stored, stored), "the stored URL itself is no echo (nothing to keep)");
    CHECK(!mqtt_url_is_echo("mqtt://192.168.1.10:1883", "mqtt://192.168.1.10:1883"),
          "without stored credentials there is nothing to keep");
    CHECK(!mqtt_url_is_echo("mqtt://x", "") && !mqtt_url_is_echo(nullptr, stored) && !mqtt_url_is_echo("mqtt://x", nullptr),
          "nothing stored, or nothing sent");
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
