// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zigbee_pool.cpp
#include "zigbee_pool.h"
#include "zigbee_mgr.h"
#include "zhc_adapter.h"
#include "zap_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdlib>

// Internal linkage — was previously extern in the public header, which let
// every TU bypass the pool mutex by indexing the array directly.
static ZapDevice* s_pool       = nullptr;
static uint16_t   s_pool_count = 0;

// Out-of-line accessors so external iteration paths keep working without
// re-exposing the raw storage. Callers must hold zigbee_pool_lock().
ZapDevice* pool_all()   { return s_pool; }
uint16_t   pool_count() { return s_pool_count; }

// Recursive so callers that hold zigbee_pool_lock() externally can still
// invoke the public functions without self-deadlocking.
static SemaphoreHandle_t s_pool_mutex = nullptr;

// Null-safe: s_pool_mutex is created in zigbee_pool_init(), which runs after
// the stores/rules bring-up (main.cpp). Early callers — e.g.
// simple_rules_resolve_names() at boot, before any device exists — must not
// crash on the not-yet-created mutex. Pre-init there is no concurrency and
// pool_count()==0, so skipping the lock is safe. (Regression fix: F35 added
// zigbee_pool_lock() to resolve_names, which runs during simple_rules_init.)
static inline void lock()   { if (s_pool_mutex) xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY); }
static inline void unlock() { if (s_pool_mutex) xSemaphoreGiveRecursive(s_pool_mutex); }

void zigbee_pool_lock()   { lock(); }
void zigbee_pool_unlock() { unlock(); }

void zigbee_pool_init() {
    s_pool_mutex = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_pool_mutex);

    s_pool = static_cast<ZapDevice*>(
        heap_caps_calloc(ZAP_MAX_DEVICES, sizeof(ZapDevice),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_pool) {
        ESP_LOGW("zigbee_pool", "PSRAM alloc failed — falling back to internal RAM");
        s_pool = static_cast<ZapDevice*>(calloc(ZAP_MAX_DEVICES, sizeof(ZapDevice)));
    }
    configASSERT(s_pool);
    ESP_LOGI("zigbee_pool", "pool alloc OK %u devices × %u bytes = %u KB",
             ZAP_MAX_DEVICES, sizeof(ZapDevice),
             (unsigned)(ZAP_MAX_DEVICES * sizeof(ZapDevice) / 1024));
}

// ── Hash tables ─────────────────────────────────────────────────────────────
// Open addressing with linear probing.
// HASH_SIZE must be a power of 2 and > ZAP_MAX_DEVICES (200).
static constexpr uint16_t HASH_SIZE  = 256;
static constexpr uint16_t HASH_EMPTY = 0xFFFF;
static constexpr uint16_t HASH_MASK  = HASH_SIZE - 1;

// Each slot stores the pool index of the entry, or HASH_EMPTY.
static uint16_t s_ieee_idx[HASH_SIZE];
static uint16_t s_nwk_idx[HASH_SIZE];

bool s_hash_dirty = true;   // force first build

static void hash_rebuild() {
    memset(s_ieee_idx, 0xFF, sizeof(s_ieee_idx));   // fill with HASH_EMPTY
    memset(s_nwk_idx,  0xFF, sizeof(s_nwk_idx));

    for (uint16_t i = 0; i < s_pool_count; i++) {
        {
            uint16_t slot = (uint16_t)((s_pool[i].ieee_addr * 2654435761ULL) >> 32) & HASH_MASK;
            while (s_ieee_idx[slot] != HASH_EMPTY) slot = (slot + 1) & HASH_MASK;
            s_ieee_idx[slot] = i;
        }
        {
            uint16_t slot = (uint16_t)(s_pool[i].nwk_addr * 2654435761U) & HASH_MASK;
            while (s_nwk_idx[slot] != HASH_EMPTY) slot = (slot + 1) & HASH_MASK;
            s_nwk_idx[slot] = i;
        }
    }

    s_hash_dirty = false;
}

// ── Public lookup functions ──────────────────────────────────────────────────
ZapDevice* pool_find_by_ieee(uint64_t ieee) {
    lock();
    if (s_hash_dirty) hash_rebuild();

    ZapDevice* result = nullptr;
    uint16_t slot = (uint16_t)((ieee * 2654435761ULL) >> 32) & HASH_MASK;
    for (uint16_t probes = 0; probes < HASH_SIZE; probes++) {
        uint16_t idx = s_ieee_idx[slot];
        if (idx == HASH_EMPTY) break;
        if (s_pool[idx].ieee_addr == ieee) { result = &s_pool[idx]; break; }
        slot = (slot + 1) & HASH_MASK;
    }
    unlock();
    return result;
}

ZapDevice* pool_find_by_nwk(uint16_t nwk) {
    lock();
    if (s_hash_dirty) hash_rebuild();

    ZapDevice* result = nullptr;
    uint16_t slot = (uint16_t)(nwk * 2654435761U) & HASH_MASK;
    for (uint16_t probes = 0; probes < HASH_SIZE; probes++) {
        uint16_t idx = s_nwk_idx[slot];
        if (idx == HASH_EMPTY) break;
        if (s_pool[idx].nwk_addr == nwk) { result = &s_pool[idx]; break; }
        slot = (slot + 1) & HASH_MASK;
    }
    unlock();
    return result;
}

// ── Add / Remove ─────────────────────────────────────────────────────────────
ZapDevice* pool_add() {
    lock();
    ZapDevice* out = nullptr;
    if (s_pool_count >= ZAP_MAX_DEVICES) {
        ESP_LOGE("zigbee_pool", "device pool full (%d)", ZAP_MAX_DEVICES);
    } else {
        out = &s_pool[s_pool_count++];
        memset(out, 0, sizeof(ZapDevice));
        s_hash_dirty = true;
    }
    unlock();
    return out;
}

void pool_remove(uint16_t idx) {
    lock();
    if (idx < s_pool_count) {
        if (idx < s_pool_count - 1)
            s_pool[idx] = s_pool[s_pool_count - 1];
        s_pool_count--;
        s_hash_dirty = true;
    }
    unlock();
}

// Force hash rebuild on the next lookup. Must be called whenever a
// field used by the hash index (currently nwk_addr) is mutated in
// place by a caller — otherwise pool_find_by_nwk won't see the new
// value and frames from the rejoined device get dropped as "unknown
// nwk". Cheap: just flips a flag; rebuild happens lazily.
void zigbee_pool_mark_dirty() {
    lock();
    s_hash_dirty = true;
    unlock();
}

// ── Restore persisted devices from NVS ──────────────────────────────
//
// z2m-equivalent: at startup z2m loads `database.db` and presents
// every paired device as already-known so reports decode from frame
// one without re-pairing. ZHAC keeps the per-device snapshot in NVS
// via `zap_store_save_device` (called from interview + rename + state
// changes), but until this restore step landed the in-memory pool was
// empty after every reboot and AF_INCOMING frames from existing
// devices dropped as "unknown ieee".
//
// Combined with the ZNP-side NV preservation (CC2652 keeps PAN ID,
// NWK key, child table, etc. across resets), this gives full z2m-
// equivalent restart semantics: the coordinator keeps the network,
// the ESP32 keeps the device snapshots, devices keep talking.
//
// Returns the device count loaded; 0 on first boot or when zap_store
// hasn't been initialised yet.
// Active count = pool slots minus tombstoned (zap_dev_is_removed)
// entries. `pool_count()` returns the raw slot count which includes
// soft-removed devices kept around for re-pair fast-path. External
// reports (HEARTBEAT, SYNC_ACK, /api/status) should use this so the
// SPA's Info-page number matches the Devices-page list (which already
// filters via `if (zap_dev_is_removed) continue`).
uint16_t pool_count_active() {
    lock();
    uint16_t n = 0;
    for (uint16_t i = 0; i < s_pool_count; i++) {
        if (!zap_dev_is_removed(&s_pool[i])) n++;
    }
    unlock();
    return n;
}

uint16_t zigbee_pool_restore_persisted() {
    if (!zap_store_is_ready()) {
        ESP_LOGW("zigbee_pool", "restore skipped — zap_store not ready");
        return 0;
    }
    lock();
    const uint16_t n = zap_store_load_devices(s_pool, ZAP_MAX_DEVICES);
    s_pool_count  = n;
    s_hash_dirty  = true;   // first lookup rebuilds ieee/nwk indices
    unlock();
    ESP_LOGI("zigbee_pool",
              "restored %u device%s from NVS — network preserved across reboot",
              n, n == 1 ? "" : "s");
    return n;
}

// Hard-remove (was in zcl_commands.cpp, where it touched s_pool / s_pool_count
// directly via the extern declarations — those have been removed so the
// implementation moves here, next to the storage it manipulates.)
bool zigbee_pool_remove(uint64_t ieee) {
    lock();
    int16_t found = -1;
    for (uint16_t i = 0; i < s_pool_count; i++) {
        if (s_pool[i].ieee_addr == ieee) { found = (int16_t)i; break; }
    }
    if (found < 0) { unlock(); return false; }
    pool_remove(static_cast<uint16_t>(found));   // swap-with-last under same lock (recursive)
    unlock();
    // Drop the adapter's cached def pointer so if a new device ever claims
    // this ieee again we don't serve the old port.
    zhac_adapter_invalidate_def_cache(ieee);
    ESP_LOGI("zigbee_pool", "pool_remove ieee=0x%016llX", (unsigned long long)ieee);
    return true;
}
