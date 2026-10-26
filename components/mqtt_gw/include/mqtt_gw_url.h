// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_gw_url.h -- the broker URL as the status shows it, and a settings write that only echoes it.
// Pure: host-tested in test/host/test_mqtt_gw_url.cpp.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

// mqtt://user:pass@host:1883 -> mqtt://host:1883, for status and logs.
inline void mqtt_url_strip_userinfo(const char* in, char* out, size_t cap) {
    out[0] = '\0';
    if (!in || !in[0]) return;
    const char* sep  = std::strstr(in, "://");
    const char* host = sep ? sep + 3 : in;
    const char* at   = nullptr;
    for (const char* p = host; *p && *p != '/'; ++p) if (*p == '@') at = p;
    std::snprintf(out, cap, "%.*s%s", (int)(host - in), in, at ? at + 1 : host);
}

// True when `sent` is the address the status shows for `stored`, its user name and password stripped:
// the client has not changed the broker, and the stored URL with them stays (review 2026-10-24, LOW round
// review M6; zhac-net-core's settings handler does the same). False when nothing would be kept.
inline bool mqtt_url_is_echo(const char* sent, const char* stored) {
    if (!sent || !stored || !stored[0] || std::strcmp(sent, stored) == 0) return false;
    char safe[128];
    mqtt_url_strip_userinfo(stored, safe, sizeof(safe));
    return std::strcmp(sent, safe) == 0;
}
