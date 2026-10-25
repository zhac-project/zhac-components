// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What zap_store needs beyond this harness's one-namespace NVS: a partition
// lookup (no zhac_data here, so the store stays on nvs and nothing is moved),
// the OTA trial state, and the entry types only its move would copy.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

esp_err_t nvs_open_from_partition(const char*, const char* ns, nvs_open_mode_t m, nvs_handle_t* out) {
    return nvs_open(ns, m, out);
}
esp_err_t nvs_flash_init_partition(const char*)  { return ESP_OK; }
esp_err_t nvs_flash_erase_partition(const char*) { return ESP_OK; }
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char*) {
    return nullptr;
}
const esp_partition_t* esp_ota_get_running_partition(void) { return nullptr; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*) { return ESP_FAIL; }

#define UNUSED_NUM(T, name)                                                         \
    esp_err_t nvs_set_##name(nvs_handle_t, const char*, T) { return ESP_OK; }      \
    esp_err_t nvs_get_##name(nvs_handle_t, const char*, T*) { return ESP_ERR_NVS_NOT_FOUND; }
UNUSED_NUM(int8_t, i8) UNUSED_NUM(int16_t, i16) UNUSED_NUM(uint32_t, u32)
UNUSED_NUM(int32_t, i32) UNUSED_NUM(uint64_t, u64) UNUSED_NUM(int64_t, i64)
#undef UNUSED_NUM
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*) { return ESP_ERR_NVS_NOT_FOUND; }
