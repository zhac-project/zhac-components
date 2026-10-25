// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host in-memory NVS + minimal esp/FreeRTOS shims for the zap_store host
// tests. Single-threaded; the FreeRTOS mutex/task calls are no-ops (the flush
// "task" never runs — the deferred writeback is drained synchronously by
// zap_store_flush_now()/flush_device()).
//
// Entries are kept per partition and namespace, typed like real NVS, so the
// move of the store to `zhac_data` (review 2026-10-24 WC-04) can be tested.
// nvs_stub_cut_after(n) models a power cut: after n more writes every write
// fails, and what was written before stays — call it again with -1 and
// re-run zap_store_init() to model the reboot.
//
// esp_rom_crc32_le is NOT defined here — zap_store's own stubs/esp_rom_crc.h
// provides a header-only (static inline) reflected CRC32, and zap_store.cpp
// picks it up in its own translation unit.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// ── In-memory store: "part/ns" → key → typed bytes ─────────────────────────
struct Entry { nvs_type_t type; std::vector<uint8_t> bytes; };
using Ns = std::map<std::string, Entry>;
static std::map<std::string, Ns>         g_ns;
static std::vector<std::string>          g_handles;          // handle - 1 → "part/ns"
static std::map<std::string, int>        g_blob_writes;      // flash-wear probe, never reset
static std::set<std::string>             g_parts;            // extra partitions that exist
static std::map<std::string, esp_err_t>  g_init_result;      // nvs_flash_init_partition answer
static esp_ota_img_states_t              g_ota = ESP_OTA_IMG_VALID;
static int                               g_writes_left = -1; // -1 = no cut
static bool                              g_open_fails = false;
static std::set<std::string>             g_part_fails;       // partitions that cannot be read at all

static std::string full(const char* part, const char* ns) { return std::string(part) + "/" + ns; }
static Ns* ns_of(nvs_handle_t h) {
    if (h == 0 || h > g_handles.size()) return nullptr;
    return &g_ns[g_handles[h - 1]];
}
static bool write_ok() {
    if (g_writes_left < 0) return true;
    if (g_writes_left == 0) return false;
    g_writes_left--;
    return true;
}

esp_err_t nvs_open_from_partition(const char* part, const char* ns, nvs_open_mode_t mode, nvs_handle_t* out) {
    if (g_open_fails || g_part_fails.count(part)) return ESP_FAIL;
    const std::string k = full(part, ns);
    if (mode == NVS_READONLY && !g_ns.count(k)) return ESP_ERR_NVS_NOT_FOUND;
    g_ns[k];
    g_handles.push_back(k);
    *out = static_cast<nvs_handle_t>(g_handles.size());
    return ESP_OK;
}
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out) {
    return nvs_open_from_partition(NVS_DEFAULT_PART_NAME, ns, mode, out);
}
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void      nvs_close(nvs_handle_t) {}

static esp_err_t put(nvs_handle_t h, const char* key, nvs_type_t type, const void* p, size_t len) {
    Ns* ns = ns_of(h);
    if (!ns) return ESP_ERR_INVALID_ARG;
    if (!write_ok()) return ESP_FAIL;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    (*ns)[key] = Entry{type, std::vector<uint8_t>(b, b + len)};
    return ESP_OK;
}
static esp_err_t get(nvs_handle_t h, const char* key, nvs_type_t type, void* out, size_t len) {
    Ns* ns = ns_of(h);
    if (!ns) return ESP_ERR_INVALID_ARG;
    auto it = ns->find(key);
    if (it == ns->end() || it->second.type != type) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, it->second.bytes.data(), len);
    return ESP_OK;
}

#define NUM(T, name, TYPE)                                                                   \
    esp_err_t nvs_set_##name(nvs_handle_t h, const char* k, T v) { return put(h, k, TYPE, &v, sizeof(v)); } \
    esp_err_t nvs_get_##name(nvs_handle_t h, const char* k, T* o) { return get(h, k, TYPE, o, sizeof(*o)); }
NUM(uint8_t, u8, NVS_TYPE_U8)     NUM(int8_t, i8, NVS_TYPE_I8)
NUM(uint16_t, u16, NVS_TYPE_U16)  NUM(int16_t, i16, NVS_TYPE_I16)
NUM(uint32_t, u32, NVS_TYPE_U32)  NUM(int32_t, i32, NVS_TYPE_I32)
NUM(uint64_t, u64, NVS_TYPE_U64)  NUM(int64_t, i64, NVS_TYPE_I64)
#undef NUM

esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* val, size_t len) {
    const esp_err_t e = put(h, key, NVS_TYPE_BLOB, val, len);
    if (e == ESP_OK) g_blob_writes[key]++;
    return e;
}
static esp_err_t get_bytes(nvs_handle_t h, const char* key, nvs_type_t type, void* out, size_t* len) {
    Ns* ns = ns_of(h);
    if (!ns) return ESP_ERR_INVALID_ARG;
    auto it = ns->find(key);
    if (it == ns->end() || it->second.type != type) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = it->second.bytes.size(); return ESP_OK; }      // size query
    if (*len < it->second.bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->second.bytes.data(), it->second.bytes.size());
    *len = it->second.bytes.size();
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
    return get_bytes(h, key, NVS_TYPE_BLOB, out, len);
}
esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* s) {
    return put(h, key, NVS_TYPE_STR, s, strlen(s) + 1);
}
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len) {
    return get_bytes(h, key, NVS_TYPE_STR, out, len);
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    Ns* ns = ns_of(h);
    if (!ns) return ESP_ERR_INVALID_ARG;
    auto it = ns->find(key);
    if (it == ns->end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!write_ok()) return ESP_FAIL;
    ns->erase(it);
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) {
    Ns* ns = ns_of(h);
    if (!ns) return ESP_ERR_INVALID_ARG;
    if (!write_ok()) return ESP_FAIL;
    ns->clear();
    return ESP_OK;
}

static void erase_part(const std::string& part) {
    for (auto& kv : g_ns)
        if (kv.first.compare(0, part.size() + 1, part + "/") == 0) kv.second.clear();
}
esp_err_t nvs_flash_init(void)  { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { erase_part(NVS_DEFAULT_PART_NAME); return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char* part) {
    auto it = g_init_result.find(part);
    return it == g_init_result.end() ? ESP_OK : it->second;
}
esp_err_t nvs_flash_erase_partition(const char* part) {
    erase_part(part);
    g_init_result.erase(part);
    return ESP_OK;
}

// ── Partitions and the OTA trial state ─────────────────────────────────────
static esp_partition_t s_part_rec{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, {}};
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char* label) {
    if (!label || !g_parts.count(label)) return nullptr;
    strncpy(s_part_rec.label, label, sizeof(s_part_rec.label) - 1);
    return &s_part_rec;
}
static esp_partition_t s_app{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, {'o', 't', 'a', '_', '0'}};
const esp_partition_t* esp_ota_get_running_partition(void) { return &s_app; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* st) {
    *st = g_ota;
    return ESP_OK;
}

// ── Iterator: snapshot of one namespace's keys at find() time ──────────────
struct StubIter { std::string ns; std::vector<std::pair<std::string, nvs_type_t>> keys; size_t pos; };
esp_err_t nvs_entry_find(const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* out) {
    *out = nullptr;
    if (g_part_fails.count(part)) return ESP_FAIL;
    auto n = g_ns.find(full(part, ns));
    if (n == g_ns.end()) return ESP_ERR_NVS_NOT_FOUND;
    auto* it = new StubIter{ns, {}, 0};
    for (auto& kv : n->second)
        if (type == NVS_TYPE_ANY || kv.second.type == type) it->keys.emplace_back(kv.first, kv.second.type);
    if (it->keys.empty()) { delete it; return ESP_ERR_NVS_NOT_FOUND; }
    *out = it; return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* iter) {
    auto* it = static_cast<StubIter*>(*iter);
    if (!it) return ESP_ERR_NVS_NOT_FOUND;
    if (++it->pos >= it->keys.size()) { delete it; *iter = nullptr; return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t iter, nvs_entry_info_t* info) {
    auto* it = static_cast<StubIter*>(iter);
    if (!it || it->pos >= it->keys.size()) return ESP_FAIL;
    memset(info, 0, sizeof(*info));
    strncpy(info->namespace_name, it->ns.c_str(), sizeof(info->namespace_name) - 1);
    strncpy(info->key, it->keys[it->pos].first.c_str(), sizeof(info->key) - 1);
    info->type = it->keys[it->pos].second;
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
extern "C" void nvs_stub_reset(void) {
    g_ns.clear(); g_handles.clear(); g_parts.clear(); g_init_result.clear(); g_part_fails.clear();
    g_ota = ESP_OTA_IMG_VALID; g_writes_left = -1; g_open_fails = false;
}
extern "C" void nvs_stub_part_fails(const char* part, bool f) {
    if (f) g_part_fails.insert(part); else g_part_fails.erase(part);
}
extern "C" void nvs_stub_set_schema(uint16_t v) {
    nvs_handle_t h;
    nvs_open("zap_v0", NVS_READWRITE, &h);
    nvs_set_u16(h, "schema_ver", v);
}
extern "C" int  nvs_stub_blob_writes(const char* key) {
    auto it = g_blob_writes.find(key);
    return it == g_blob_writes.end() ? 0 : it->second;
}
extern "C" void nvs_stub_add_partition(const char* label)            { g_parts.insert(label); }
extern "C" void nvs_stub_drop_partition(const char* label)           { g_parts.erase(label); }
extern "C" void nvs_stub_init_result(const char* part, esp_err_t e)  { g_init_result[part] = e; }
extern "C" void nvs_stub_ota_state(int st)                           { g_ota = static_cast<esp_ota_img_states_t>(st); }
extern "C" void nvs_stub_cut_after(int writes)                       { g_writes_left = writes; }
extern "C" void nvs_stub_open_fails(bool f)                          { g_open_fails = f; }
extern "C" int  nvs_stub_count(const char* part, const char* ns) {
    auto it = g_ns.find(full(part, ns));
    return it == g_ns.end() ? 0 : static_cast<int>(it->second.size());
}
