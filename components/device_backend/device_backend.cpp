// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/device_backend/device_backend.cpp
#include "device_backend.h"
#include "esp_log.h"

static const char* TAG = "dev_backend";

static DeviceBackend* s_backends[DEVICE_BACKEND_MAX] = {};
static uint8_t        s_backend_count = 0;

bool device_backend_register(DeviceBackend* b) {
    if (!b) return false;
    if (s_backend_count >= DEVICE_BACKEND_MAX) {
        ESP_LOGE(TAG, "backend registry full (%d)", DEVICE_BACKEND_MAX);
        return false;
    }
    for (uint8_t i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->protocol == b->protocol) {
            ESP_LOGW(TAG, "backend %s already registered for protocol %d",
                     b->name, b->protocol);
            return false;
        }
    }
    s_backends[s_backend_count++] = b;
    ESP_LOGI(TAG, "registered backend '%s' (proto=%d)", b->name, b->protocol);
    return true;
}

DeviceBackend* device_backend_find(NcpProtocol proto) {
    for (uint8_t i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->protocol == proto) return s_backends[i];
    }
    return nullptr;
}

DeviceBackend* device_backend_find_by_ieee(uint64_t ieee) {
    ZapDevice tmp{};
    for (uint8_t i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->get_device && s_backends[i]->get_device(ieee, &tmp)) {
            return s_backends[i];
        }
    }
    return nullptr;
}

uint8_t device_backend_count() {
    return s_backend_count;
}

DeviceBackend* device_backend_get(uint8_t index) {
    if (index >= s_backend_count) return nullptr;
    return s_backends[index];
}
