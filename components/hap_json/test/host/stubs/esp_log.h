// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test esp_log shim for the hap_json device-list paging suite. Mirrors
// the zap_common host shim: ESP_LOGE bumps a global counter so the test can
// assert the "encode overflow" path actually logged; the rest are stderr
// printfs (quietened to debug/verbose no-ops).
#pragma once
#include <stdio.h>

extern int g_esp_loge_count;

#define ESP_LOGE(tag, fmt, ...) do { g_esp_loge_count++;                       \
    fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
