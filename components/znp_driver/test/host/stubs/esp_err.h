// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host shim for esp_err — only the handful of names the driver/uart.h stub
// surface needs. The transport TUs that would call these are out of scope.
#pragma once
typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL -1
