// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zap_store/zap_store.cpp
#include "zap_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_checked.h"
#include "nvs_namespaces.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG     = "zap_store";
// P2 (FINDINGS §8.5): use the central namespace registry rather than an
// inlined "zap_v0" literal — one place audits every NVS namespace string.
static const char* NVS_NS  = zap_nvs::DEVICE_POOL;
static bool        s_ready = false;

// F16 (FINDINGS.md): serialise save (flush task) and delete (dispatch task) —
// previously unsynchronised, a latent race on the slot table — and back save
// with an in-RAM IEEE→slot index so a bulk interview is O(n), not O(n²) NVS
// blob scans. The index mirrors the on-disk slot order; delete invalidates it
// (rebuilt lazily on the next save).
static SemaphoreHandle_t s_store_mutex = nullptr;
static uint64_t          s_idx_ieee[ZAP_MAX_DEVICES];
static uint16_t          s_idx_count   = 0;
static bool              s_idx_built   = false;

static inline void store_lock()   { if (s_store_mutex) xSemaphoreTake(s_store_mutex, portMAX_DELAY); }
static inline void store_unlock() { if (s_store_mutex) xSemaphoreGive(s_store_mutex); }

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

// F16: (re)build the IEEE index from NVS — one O(n) scan. Caller holds the
// store lock and passes an open handle.
static void index_build_locked(nvs_handle_t h) {
    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);
    if (count > ZAP_MAX_DEVICES) count = ZAP_MAX_DEVICES;
    s_idx_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        char key[8]; dev_key(key, i);
        ZapDevice tmp{}; size_t sz = sizeof(ZapDevice);
        s_idx_ieee[i] = (nvs_get_blob(h, key, &tmp, &sz) == ESP_OK) ? tmp.ieee_addr : 0;
        s_idx_count = static_cast<uint16_t>(i + 1);
    }
    s_idx_built = true;
}

static int16_t index_find_locked(uint64_t ieee) {
    for (uint16_t i = 0; i < s_idx_count; i++)
        if (s_idx_ieee[i] == ieee) return static_cast<int16_t>(i);
    return -1;
}

// ── CRC32 integrity ──────────────────────────────────────────────────────

// Compute CRC32 over a ZapDevice whose crc32 field is ALREADY zeroed by the
// caller. No defensive stack copy — the save path holds a crc-zeroed copy
// and the verify path zeroes the field around the check. Keeping the copy
// out of here avoids a redundant 522 B stack frame + memcpy per call
// (FINDINGS §8.4).
static inline uint32_t zap_device_crc_zeroed(const ZapDevice* d) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(d), sizeof(*d));
}

// Compute CRC32 over an arbitrary ZapDevice (crc32 field excluded). Used by
// the load path, where the in-RAM record carries the STORED crc32 that must
// be preserved for the equality check — so this variant zeroes a copy.
static uint32_t zap_device_crc(const ZapDevice* d) {
    ZapDevice tmp;
    memcpy(&tmp, d, sizeof(tmp));
    tmp.crc32 = 0;
    return zap_device_crc_zeroed(&tmp);
}

// Schema version stored in NVS alongside data.
// Bump this constant AND migrate/erase old data whenever ZapDevice layout changes.
// v4 → v5: added CRC32 integrity checking on device records.
// v5 → v6: added lifecycle state fields (interview/support/configure_state,
//          configure_attempts) to ZapDevice; struct grew 518 → 522 bytes.
static constexpr uint16_t ZAP_STORE_SCHEMA_VERSION = 6;

void zap_store_init() {
    if (!s_store_mutex) s_store_mutex = xSemaphoreCreateMutex();   // F16
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
                esp_err_t acc = ESP_OK;
                nvs_seq(&acc, nvs_erase_all(h), TAG, "erase_all devpool");
                if (acc == ESP_OK) {
                    // Version marker only after a clean wipe — writing the
                    // new version over a failed erase would let stale-layout
                    // blobs sit behind the current marker.
                    nvs_seq(&acc, nvs_set_u16(h, "schema_ver",
                                              ZAP_STORE_SCHEMA_VERSION),
                            TAG, "set_u16 schema_ver");
                    nvs_seq(&acc, nvs_commit(h), TAG, "commit schema_ver");
                }
                if (acc != ESP_OK)
                    ESP_LOGE(TAG, "schema wipe incomplete — will retry next boot");
            }
        } else {
            // First boot with versioning — write current version
            esp_err_t acc = ESP_OK;
            nvs_seq(&acc, nvs_set_u16(h, "schema_ver", ZAP_STORE_SCHEMA_VERSION),
                    TAG, "set_u16 schema_ver");
            nvs_seq(&acc, nvs_commit(h), TAG, "commit schema_ver");
        }
        nvs_close(h);
    }

    s_ready = true;
    ESP_LOGI(TAG, "zap_store init OK (schema=%u)", ZAP_STORE_SCHEMA_VERSION);
}

bool zap_store_is_ready() { return s_ready; }

// ── Save device with CRC32 ───────────────────────────────────────────────
// Resolves the slot via the in-RAM IEEE index (built once from NVS), writes
// with a CRC32 checksum. F16 (FINDINGS.md): the store mutex serialises this
// with zap_store_delete_device (cross-task — flush vs dispatch), and the index
// removes the old per-save O(n) NVS scan (a bulk interview was O(n²)). The
// never-implemented A/B-snapshot + journal scheme was removed 2026-05-29; NVS
// is the store.

bool zap_store_save_device(const ZapDevice* dev) {
    if (!dev) return false;

    store_lock();   // F16: serialise with delete + protect the IEEE index
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (!h) { store_unlock(); return false; }

    uint16_t count = 0;
    nvs_get_u16(h, "cnt", &count);

    // F16 (FINDINGS.md): resolve the slot via the in-RAM IEEE index instead of
    // an O(n) NVS blob scan per save (which made a bulk interview O(n²)). Built
    // once from NVS, kept in sync below; delete invalidates it.
    char key[8];
    if (!s_idx_built) index_build_locked(h);
    int16_t found_idx = index_find_locked(dev->ieee_addr);

    // Capacity guard (FINDINGS §8.1): a NEW device (found_idx < 0) at a full
    // store would take idx = count = ZAP_MAX_DEVICES, write blob "d00c8",
    // and bump cnt to 201 — but the index array and every load/delete loop
    // cap at ZAP_MAX_DEVICES, so that slot is unreferenced. Each later save
    // of the same IEEE re-misses the index, re-appends another 522 B blob at
    // an ever-higher idx, and re-bumps cnt: unbounded NVS growth until the
    // partition is exhausted. Reject BEFORE any write or count bump. An
    // existing device (found_idx >= 0) is always an in-place rewrite and is
    // never rejected, even at capacity.
    if (found_idx < 0 && count >= ZAP_MAX_DEVICES) {
        ESP_LOGE(TAG, "device store FULL (%u/%u) — rejecting save ieee=0x%016llx",
                 count, (unsigned)ZAP_MAX_DEVICES,
                 (unsigned long long)dev->ieee_addr);
        nvs_close(h);
        store_unlock();
        return false;
    }

    uint16_t idx = (found_idx >= 0) ? static_cast<uint16_t>(found_idx) : count;

    // Compute CRC32 before saving (zero the field so it's excluded from the
    // checksum, then CRC the zeroed copy in place — no second stack copy).
    ZapDevice saved_dev;
    memcpy(&saved_dev, dev, sizeof(saved_dev));
    saved_dev.crc32 = 0;
    saved_dev.crc32 = zap_device_crc_zeroed(&saved_dev);

    // For a new device, bump "cnt" BEFORE writing the blob so that a power
    // loss between the two writes leaves a referenced-but-unwritten slot
    // (load_devices skips it via the nvs_get_blob != ESP_OK branch or the
    // CRC32 check) rather than a written-but-unreferenced blob (which the
    // load loop would never read because the count stops short). Without
    // this order the new-device record is silently lost on crash —
    // device rejoins + re-interviews, but friendly name + configure state
    // are gone until the new interview lands.
    dev_key(key, idx);
    if (found_idx < 0) {
        esp_err_t cnt_err = nvs_set_u16(h, "cnt", count + 1);
        if (cnt_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_u16 cnt: %s", esp_err_to_name(cnt_err));
            nvs_close(h);
            store_unlock();
            return false;
        }
    }
    esp_err_t err = nvs_set_blob(h, key, &saved_dev, sizeof(saved_dev));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        store_unlock();
        return false;
    }

    esp_err_t acc = ESP_OK;
    nvs_seq(&acc, nvs_commit(h), TAG, "commit devpool save");
    nvs_close(h);

    // F16: keep the index in sync on success. NVS commits atomically, so on
    // failure NVS is unchanged and the index must stay as-is.
    if (acc == ESP_OK && idx < ZAP_MAX_DEVICES) {
        s_idx_ieee[idx] = dev->ieee_addr;
        if (idx >= s_idx_count) s_idx_count = static_cast<uint16_t>(idx + 1);
    }
    store_unlock();

    // Success log gated on the commit — a failed save must not read as
    // "Device saved" in the log while the function returns false.
    if (acc == ESP_OK)
        ESP_LOGI(TAG, "Device saved idx=%u ieee=0x%016llx", idx, dev->ieee_addr);
    return acc == ESP_OK;
}

// ── Delete device — atomic read-all / erase-all / rewrite / commit ───────
// Old approach (shift-in-place with single commit) was vulnerable to power
// loss producing duplicate entries. New approach: read everything into RAM,
// erase all keys, write back compacted set, single commit.

bool zap_store_delete_device(uint64_t ieee) {
    bool      ok    = false;
    uint16_t  count = 0;
    ZapDevice* devs = nullptr;
    store_lock();   // F16: serialise with save + protect the IEEE index
    do {
        // 1. Read the stored count first, then size the scratch buffer to
        //    the ACTUAL device count — not ZAP_MAX_DEVICES (FINDINGS §8.2).
        //    The old fixed 200 × 522 B (~102 KB) alloc could fail on a
        //    fragmented heap with PSRAM unavailable, making delete
        //    impossible. cnt is read under the store lock to stay
        //    consistent with the rewrite below.
        nvs_handle_t h = open_ns(NVS_READONLY);
        if (!h) break;
        nvs_get_u16(h, "cnt", &count);
        if (count > ZAP_MAX_DEVICES) count = ZAP_MAX_DEVICES;
        if (count == 0) { nvs_close(h); break; }  // empty store — nothing to delete

        // ZapDevice is ~522 B. Prefer PSRAM (P4 has it), fall back to
        // internal heap. Sized to `count`, so a near-empty store costs
        // a few hundred bytes rather than ~102 KB.
        devs = static_cast<ZapDevice*>(heap_caps_calloc(
            count, sizeof(ZapDevice), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!devs) devs = static_cast<ZapDevice*>(
            std::calloc(count, sizeof(ZapDevice)));
        if (!devs) {
            ESP_LOGE(TAG, "delete: alloc %u×%u failed",
                     (unsigned)count, (unsigned)sizeof(ZapDevice));
            nvs_close(h);
            break;
        }

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

        // Any erase/rewrite failure aborts WITHOUT the commit: a partial
        // rewrite must not become durable (committing after a failed
        // erase/set could persist a half-compacted slot table).
        esp_err_t acc = ESP_OK;
        uint16_t old_count = count + 1;  // +1 for the removed device
        for (uint16_t i = 0; i < old_count && acc == ESP_OK; i++) {
            char key[8]; dev_key(key, i);
            nvs_seq(&acc, nvs_erase_key(h, key), TAG, "erase_key devpool");
        }
        for (uint16_t i = 0; i < count && acc == ESP_OK; i++) {
            char key[8]; dev_key(key, i);
            nvs_seq(&acc, nvs_set_blob(h, key, &devs[i], sizeof(ZapDevice)),
                    TAG, "set_blob devpool");
        }
        if (acc == ESP_OK)
            nvs_seq(&acc, nvs_set_u16(h, "cnt", count), TAG, "set_u16 cnt");
        if (acc == ESP_OK)
            nvs_seq(&acc, nvs_commit(h), TAG, "commit devpool delete");
        nvs_close(h);

        if (acc != ESP_OK) break;
        ok = true;
    } while (0);

    s_idx_built = false;   // F16: slot order changed — index rebuilt on next save
    store_unlock();
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
    // FINDINGS §8.3: validate the destination, and take the store lock so the
    // multi-blob scan is consistent vs a concurrent save/delete once the flush
    // task is running (load is normally a cold boot path, but the flush task
    // can write before the boot relist completes). Lock order: store_lock is
    // the INNERMOST lock — the flush layer (zap_store_flush.cpp) always
    // releases its own s_mtx before calling into save/delete, so taking
    // s_store_mutex here cannot invert against it.
    if (!pool || max_count == 0) {
        ESP_LOGE(TAG, "load_devices: null pool or zero capacity");
        return 0;
    }

    store_lock();
    nvs_handle_t h = open_ns(NVS_READONLY);
    if (!h) { store_unlock(); return 0; }

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
    store_unlock();
    ESP_LOGI(TAG, "Loaded %u devices from NVS", loaded);
    return loaded;
}

