// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// device_shadow persists its NVS schema version under a u8 "ver" key. The
// verbatim-copied nvs_stub.cpp (shared with the zap_store harness) only
// implements the blob + u16 API zap_store uses, so the u8 pair lives here in
// its own tiny in-memory map. Kept separate so nvs_stub.cpp stays a byte-for-
// byte copy of the proven store shim.
//
// NOTE: this map is intentionally NOT cleared by nvs_stub_reset()/nvs_erase_all
// — it only ever holds the shadow "ver" marker, and leaving it set across a
// blob wipe faithfully models a reformat-preserving version bump (device_shadow
// re-stamps it after erase_all anyway).
#include "nvs.h"
#include <map>
#include <string>

static std::map<std::string, uint8_t> g_u8;

esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t v) {
    g_u8[key] = v;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out) {
    auto it = g_u8.find(key);
    if (it == g_u8.end()) return ESP_ERR_NVS_NOT_FOUND;
    *out = it->second;
    return ESP_OK;
}
