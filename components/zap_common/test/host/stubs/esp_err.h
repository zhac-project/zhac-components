// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// Host-test esp_err shim — just enough for nvs_checked.h.
#pragma once

typedef int esp_err_t;

#define ESP_OK   0
#define ESP_FAIL -1

static inline const char* esp_err_to_name(esp_err_t e) {
    return (e == ESP_OK) ? "ESP_OK" : "ESP_FAIL(stub)";
}
