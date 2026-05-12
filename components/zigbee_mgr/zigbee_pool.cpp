// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zigbee_pool.cpp
#include "zigbee_pool.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdlib>

ZapDevice* s_pool       = nullptr;
uint16_t   s_pool_count = 0;

// Recursive so callers that hold zigbee_pool_lock() externally can still
// invoke the public functions without self-deadlocking.
static SemaphoreHandle_t s_pool_mutex = nullptr;

static inline void lock()   { xSemaphoreTakeRecursive(s_pool_mutex, portMAX_DELAY); }
static inline void unlock() { xSemaphoreGiveRecursive(s_pool_mutex); }

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
