// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host in-memory NVS + minimal esp/FreeRTOS shims for the zap_store host
// tests. Reused from the rule_store host harness (the two components share the
// same NVS-backed store + flush-task shape). Single-threaded; the FreeRTOS
// mutex/task calls are no-ops (the flush "task" never runs — the deferred
// writeback is drained synchronously by zap_store_flush_now()/flush_device()).
//
// esp_rom_crc32_le is NOT defined here — zap_store's own stubs/esp_rom_crc.h
// provides a header-only (static inline) reflected CRC32, and zap_store.cpp
// picks it up in its own translation unit.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

// ── In-memory store (one namespace; the tests use only zap_store's) ─────────
static std::map<std::string, std::vector<uint8_t>> g_blobs;
static std::map<std::string, uint16_t>             g_u16;

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out) { *out = 1; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void      nvs_close(nvs_handle_t) {}

esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* val, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(val);
    g_blobs[key] = std::vector<uint8_t>(p, p + len);
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* out, size_t* len) {
    auto it = g_blobs.find(key);
    if (it == g_blobs.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = it->second.size(); return ESP_OK; }      // size query
    if (*len < it->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->second.data(), it->second.size());
    *len = it->second.size();
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t, const char* key, uint16_t v) { g_u16[key] = v; return ESP_OK; }
esp_err_t nvs_get_u16(nvs_handle_t, const char* key, uint16_t* out) {
    auto it = g_u16.find(key);
    if (it == g_u16.end()) return ESP_ERR_NVS_NOT_FOUND;
    *out = it->second; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t, const char* key) {
    auto it = g_blobs.find(key);
    if (it == g_blobs.end()) return ESP_ERR_NVS_NOT_FOUND;
    g_blobs.erase(it); return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t) { g_blobs.clear(); g_u16.clear(); return ESP_OK; }
esp_err_t nvs_flash_init(void)  { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { g_blobs.clear(); g_u16.clear(); return ESP_OK; }

// ── Iterator: snapshot of blob keys at find() time ─────────────────────────
struct StubIter { std::vector<std::string> keys; size_t pos; };
esp_err_t nvs_entry_find(const char*, const char*, nvs_type_t, nvs_iterator_t* out) {
    auto* it = new StubIter();
    for (auto& kv : g_blobs) it->keys.push_back(kv.first);
    it->pos = 0;
    if (it->keys.empty()) { delete it; *out = nullptr; return ESP_ERR_NVS_NOT_FOUND; }
    *out = it; return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* iter) {
    auto* it = static_cast<StubIter*>(*iter);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    it->pos++;
    return (it->pos >= it->keys.size()) ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t iter, nvs_entry_info_t* info) {
    auto* it = static_cast<StubIter*>(iter);
    if (!it || it->pos >= it->keys.size()) return ESP_FAIL;
    memset(info, 0, sizeof(*info));
    strncpy(info->key, it->keys[it->pos].c_str(), sizeof(info->key) - 1);
    info->type = NVS_TYPE_BLOB;
    return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t iter) { delete static_cast<StubIter*>(iter); }

// ── esp_timer / FreeRTOS ───────────────────────────────────────────────────
static int64_t g_time_us = 0;
int64_t esp_timer_get_time(void) { return (g_time_us += 1000); }   // +1 ms per call

static int s_mutex_obj = 0;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex_obj; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
BaseType_t xTaskCreate(void (*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*) {
    return pdPASS;   // the flush task never runs; flush_now()/flush_device() are synchronous
}
void vTaskDelay(TickType_t) {}

// ── Test helpers ───────────────────────────────────────────────────────────
extern "C" void nvs_stub_reset(void)            { g_blobs.clear(); g_u16.clear(); }
extern "C" void nvs_stub_set_schema(uint16_t v) { g_u16["schema_ver"] = v; }
