// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// Host-test esp_log shim — stderr printf plus an ESP_LOGE call counter
// so the suite can assert nvs_seq logs once per failing op. (Local to
// zap_common: the shared simple_rules shims have no counter and no
// esp_err.h.)
#pragma once
#include <stdio.h>

extern int g_esp_loge_count;

#define ESP_LOGE(tag, fmt, ...) do { g_esp_loge_count++;                       \
    fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
