// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zap_store/zap_store.cpp
#include "zap_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG     = "zap_store";
static const char* NVS_NS  = "zap_v0";
static bool        s_ready = false;

static void dev_key(char* out, uint16_t idx) {
    snprintf(out, 8, "d%04x", idx);
}

static nvs_handle_t open_ns(nvs_open_mode_t mode) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, mode, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

// ── CRC32 integrity ──────────────────────────────────────────────────────

// Compute CRC32 over the entire ZapDevice with the crc32 field zeroed.
// Mirrors the pattern used in rule_store for RuleSlot integrity.
static uint32_t zap_device_crc(const ZapDevice* d) {
    ZapDevice tmp;
    memcpy(&tmp, d, sizeof(tmp));
    tmp.crc32 = 0;
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&tmp), sizeof(tmp));
}

// Schema version stored in NVS alongside data.
// Bump this constant AND migrate/erase old data whenever ZapDevice layout changes.
// v4 → v5: added CRC32 integrity checking on device records.
// v5 → v6: added lifecycle state fields (interview/support/configure_state,
//          configure_attempts) to ZapDevice; struct grew 518 → 522 bytes.
static constexpr uint16_t ZAP_STORE_SCHEMA_VERSION = 6;

void zap_store_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Schema version check: detect struct layout changes between firmware versions.
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h) {
        uint16_t stored_ver = 0;
        if (nvs_get_u16(h, "schema_ver", &stored_ver) == ESP_OK) {
            if (stored_ver != ZAP_STORE_SCHEMA_VERSION) {
                ESP_LOGW(TAG, "NVS schema version mismatch: stored=%u current=%u — "
                         "erasing device store",
                         stored_ver, ZAP_STORE_SCHEMA_VERSION);
                nvs_erase_all(h);
                nvs_set_u16(h, "schema_ver", ZAP_STORE_SCHEMA_VERSION);
                nvs_commit(h);
            }
        } else {
            // First boot with versioning — write current version
            nvs_set_u16(h, "schema_ver", ZAP_STORE_SCHEMA_VERSION);
            nvs_commit(h);
        }
        nvs_close(h);
    }

    s_ready = true;
    ESP_LOGI(TAG, "zap_store init OK (NVS v0, schema=%u)", ZAP_STORE_SCHEMA_VERSION);
}

bool zap_store_is_ready() { return s_ready; }

// ── Save device with CRC32 ───────────────────────────────────────────────
// Scans existing entries to find matching IEEE, writes with CRC checksum.
// NOTE: O(n) NVS reads for the scan. For bulk interviews this creates O(n²)
// total I/O. A future optimisation would cache an IEEE→index hash.

bool zap_store_save_device(const ZapDevice* dev) {
    if (!dev) return false;

    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) return false;

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);

    char key[8];
    int16_t found_idx = -1;
    for (uint16_t i = 0; i < count; i++) {
        dev_key(key, i);
        ZapDevice tmp{};
        size_t sz = sizeof(ZapDevice);
        if (nvs_get_blob(h, key, &tmp, &sz) == ESP_OK) {
            if (tmp.ieee_addr == dev->ieee_addr) {
                found_idx = static_cast<int16_t>(i);
                break;
            }
        }
    }

    uint16_t idx = (found_idx >= 0) ? static_cast<uint16_t>(found_idx) : count;

    // Compute CRC32 before saving (zero the field so it's excluded from checksum).
    ZapDevice saved_dev;
    memcpy(&saved_dev, dev, sizeof(saved_dev));
    saved_dev.crc32 = 0;
    saved_dev.crc32 = zap_device_crc(&saved_dev);

    dev_key(key, idx);
    esp_err_t err = nvs_set_blob(h, key, &saved_dev, sizeof(saved_dev));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    if (found_idx < 0) {
        count++;
        nvs_set_u16(h, "cnt", count);
    }

    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Device saved idx=%u ieee=0x%016llx", idx, dev->ieee_addr);
    return err == ESP_OK;
}

// ── Delete device — atomic read-all / erase-all / rewrite / commit ───────
// Old approach (shift-in-place with single commit) was vulnerable to power
// loss producing duplicate entries. New approach: read everything into RAM,
// erase all keys, write back compacted set, single commit.

bool zap_store_delete_device(uint64_t ieee) {
    // ZapDevice is ~450 B; ZAP_MAX_DEVICES=200 would push 90 KB onto the
    // ~4 KB hap_slave stack. Prefer PSRAM (P4 has it), fall back to
    // internal heap.
    ZapDevice* devs = static_cast<ZapDevice*>(heap_caps_calloc(
        ZAP_MAX_DEVICES, sizeof(ZapDevice),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!devs) devs = static_cast<ZapDevice*>(
        std::calloc(ZAP_MAX_DEVICES, sizeof(ZapDevice)));
    if (!devs) {
        ESP_LOGE(TAG, "delete: alloc %u×%u failed",
                 (unsigned)ZAP_MAX_DEVICES, (unsigned)sizeof(ZapDevice));
        return false;
    }

    bool ok = false;
    uint16_t count = 0;
    do {
        // 1. Read all devices into the scratch buffer.
        nvs_handle_t h = open_ns(NVS_READONLY);
        if (!h) break;
        nvs_get_u16(h, "cnt", &count);
        if (count > ZAP_MAX_DEVICES) count = ZAP_MAX_DEVICES;
        bool read_ok = true;
        for (uint16_t i = 0; i < count; i++) {
            char key[8]; dev_key(key, i);
            size_t sz = sizeof(ZapDevice);
            if (nvs_get_blob(h, key, &devs[i], &sz) != ESP_OK) {
                read_ok = false;
                break;
            }
        }
        nvs_close(h);
        if (!read_ok) break;

        // 2. Find and remove (swap-with-last to keep array compact).
        int16_t found_idx = -1;
        for (uint16_t i = 0; i < count; i++) {
            if (devs[i].ieee_addr == ieee) { found_idx = (int16_t)i; break; }
        }
        if (found_idx < 0) break;

        if (found_idx < (int16_t)(count - 1))
            devs[found_idx] = devs[count - 1];
        count--;

        // 3. Erase ALL old device keys + count, then write back compacted
        //    set with a single commit at the end.
        h = open_ns(NVS_READWRITE);
        if (!h) break;

        uint16_t old_count = count + 1;  // +1 for the removed device
        for (uint16_t i = 0; i < old_count; i++) {
            char key[8]; dev_key(key, i);
            nvs_erase_key(h, key);
        }
        for (uint16_t i = 0; i < count; i++) {
            char key[8]; dev_key(key, i);
            nvs_set_blob(h, key, &devs[i], sizeof(ZapDevice));
        }
        nvs_set_u16(h, "cnt", count);

        esp_err_t err = nvs_commit(h);
        nvs_close(h);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed during delete: %s",
                     esp_err_to_name(err));
            break;
        }
        ok = true;
    } while (0);

    free(devs);
    if (ok) {
        ESP_LOGI(TAG, "Device deleted ieee=0x%016llx remaining=%u",
                 (unsigned long long)ieee, count);
    }
    return ok;
}

// ── Load devices with CRC32 validation ───────────────────────────────────
// Each loaded device is checked for CRC integrity. Corrupted entries are
// skipped and logged — the system continues with the remaining valid devices.

uint16_t zap_store_load_devices(ZapDevice* pool, uint16_t max_count) {
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (!h) return 0;

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count > max_count) count = max_count;

    uint16_t loaded = 0;
    for (uint16_t i = 0; i < count; i++) {
        char key[8]; dev_key(key, i);
        size_t sz = sizeof(ZapDevice);
        if (nvs_get_blob(h, key, &pool[loaded], &sz) != ESP_OK) continue;

        // CRC32 validation
        uint32_t expected = zap_device_crc(&pool[loaded]);
        if (pool[loaded].crc32 != expected) {
            ESP_LOGE(TAG, "CRC32 mismatch idx=%u ieee=0x%016llx "
                     "stored=0x%08lx calc=0x%08lx — skipping",
                     i, (unsigned long long)pool[loaded].ieee_addr,
                     (unsigned long)pool[loaded].crc32, (unsigned long)expected);
            continue;
        }

        loaded++;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %u devices from NVS", loaded);
    return loaded;
}
