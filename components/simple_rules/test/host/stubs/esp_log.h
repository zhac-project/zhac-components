// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test esp_log shim — E/W/I print to stderr, D is dropped; every line is
// also recorded with its level, so a test can check the level a message went
// out at (stub_log_count). Header-only: device_cmd's and event_bus's host
// harnesses borrow this file without our host_stubs.cpp.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>

struct StubLogLine { char level; char text[160]; };
inline StubLogLine g_stub_log[256];
inline int         g_stub_log_n = 0;

__attribute__((format(printf, 3, 4)))
inline void stub_log(char level, const char* tag, const char* fmt, ...) {
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (level != 'D') fprintf(stderr, "%c (%s) %s\n", level, tag, text);
    StubLogLine& l = g_stub_log[g_stub_log_n++ % 256];
    l.level = level;
    snprintf(l.text, sizeof(l.text), "%s", text);
}

// Lines logged at `level` whose text contains `needle` (last 256 lines).
inline int stub_log_count(char level, const char* needle) {
    int n = 0;
    for (int i = 0; i < g_stub_log_n && i < 256; i++)
        if (g_stub_log[i].level == level && std::strstr(g_stub_log[i].text, needle)) n++;
    return n;
}

#define ESP_LOGE(tag, fmt, ...) stub_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) stub_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) stub_log('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
