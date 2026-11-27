// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) (printf("W %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define ESP_LOGI(tag, ...) (printf("I %s: ", tag), printf(__VA_ARGS__), printf("\n"))
#define ESP_LOGE(tag, ...) (printf("E %s: ", tag), printf(__VA_ARGS__), printf("\n"))
