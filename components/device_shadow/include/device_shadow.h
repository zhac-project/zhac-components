// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "zap_common.h"
#include "zcl_attribute.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <cstddef>
#include <cstdint>
#include <stdbool.h>

// Concurrency: event-bus FILTERS must not call shadow APIs that emit ZCL_ATTR
// (process / config setters) — filters run in the publisher's task and
// self-deadlock on the shadow's outer emit lock; drain-side HANDLERS are fine.

// ── ShadowAttr — one cached attribute slot (84 bytes, string-keyed) ──────
// Schema v6: ATTR_KEY_MAX widened 20→28 + ATTR_STR_MAX widened 32→48 so
// long z2m action labels (`brightness_step_up_color_temperature_step_up`,
// 43 chars) and longer attribute names (`color_temperature_startup`,
// 25 chars) fit untruncated. Older cache wiped on boot via
// NVS_SHADOW_VERSION bump (5→6). Total grows 60→84 bytes — 32 attrs/dev
// × 200 devs = 537 KB in PSRAM at peak (was 384 KB).
struct __attribute__((packed)) ShadowAttr {
    char     key[ATTR_KEY_MAX];   // 0-27   null-terminated attribute name
    uint8_t  val_type;            // 28     ValType
    uint8_t  flags;               // 29     reserved (0 today; SHA-F4 will use bit 0 for FLOAT_X100)
    uint16_t _pad;                // 30-31
    union {
        int32_t int_val;                // 32-35 when val_type==VAL_INT/BOOL
        char    str_val[ATTR_STR_MAX];  // 32-79 when val_type==VAL_STR
    };
    uint32_t ts;                  // 80-83  seconds since boot; 0 = never seen
};
static_assert(sizeof(ShadowAttr) == 84);
static_assert(offsetof(ShadowAttr, val_type) == ATTR_KEY_MAX);
static_assert(offsetof(ShadowAttr, int_val)  == 32);
static_assert(offsetof(ShadowAttr, ts)       == 80);

// ── DeviceConfig — per-device middleware configuration ────────────────────
// Filter arrays are now string-keyed too. Each slot stores a short attribute
// name (e.g. "linkquality"). Persisted in NVS under the shadow schema.
static constexpr uint8_t DEVICE_CONFIG_FILTER_MAX = 8;

struct __attribute__((packed)) DeviceConfig {
    uint32_t debounce_ms;              // 0 = disabled
    uint32_t throttle_ms;              // 0 = disabled
    bool     last_seen_enabled;        // update ZapDevice.last_seen on each message
    bool     optimistic;               // update cache on successful command send
    uint8_t  filtered_count;           // number of entries in filtered[]
    uint8_t  debounce_ignore_count;    // number of entries in debounce_ignore[]
    char     filtered[DEVICE_CONFIG_FILTER_MAX][ATTR_KEY_MAX];
    char     debounce_ignore[DEVICE_CONFIG_FILTER_MAX][ATTR_KEY_MAX];
    uint16_t occupancy_timeout_s;      // 0 = disabled, range 10–3600
    uint8_t  _cfg_pad[2];              // alignment padding
};

// ── Runtime-only pending state for debounce (not persisted) ──────────────
struct PendingState {
    ZclAttribute pending[32];
    uint8_t      pending_count;
};

// ── Runtime shadow entry (PSRAM, partially persisted) ────────────────────
struct DeviceShadowEntry {
    uint64_t      ieee;
    ShadowAttr    attrs[32];        // last-known state cache (persisted in NVS)
    uint8_t       attr_count;
    DeviceConfig  config;           // persisted in NVS
    TimerHandle_t debounce_timer;   // runtime-only
    TimerHandle_t occupancy_timer;  // runtime-only, occupancy TTL
    PendingState  pending;          // runtime-only
    uint32_t      throttle_last_ms; // runtime-only, milliseconds
    uint32_t      nvs_last_write_s; // runtime-only, seconds since boot
    volatile bool debounce_pending_flush; // set by timer cb if queue full (fallback)
    volatile bool occupancy_timeout_pending; // set by timer cb if queue full (fallback)
    bool          nvs_dirty;        // runtime-only — skipped NVS write pending flush
    bool          nvs_force;        // runtime-only — next sweep writes regardless of interval
    uint32_t      cfg_crc;          // F26: crc32 of last-persisted config (dedupe writes)
    bool          cfg_crc_valid;    // F26: true once cfg_crc reflects a persisted config
};

// ── Public API ────────────────────────────────────────────────────────────

// Lifecycle — init only (mutex, queues, NVS version guard, shadow alloc,
// task spawn). Does NOT load device entries. Call after zap_store_init().
// Pair with device_shadow_restore_from_pool() once the device pool has
// been populated (e.g. via zigbee_pool_restore_persisted()) to seed the
// per-device shadow entries from the persisted NVS attr cache.
void device_shadow_init();

// Restore shadow entries (config + attrs + last_seen) for each device
// in the supplied pool slice. Caller must hold the pool lock for the
// duration of the call (the pool array is iterated in place). Table
// mutation is serialised internally against the (already-running)
// task_shadow housekeeping loop; NVS reads and the DEVICE_JOIN event
// fired per restored device both happen outside the shadow lock.
// Boot-path only: must not run concurrently with itself. Returns the
// number of entries restored.
uint16_t device_shadow_restore_from_pool(const ZapDevice* pool, uint16_t count);

// Called by zigbee_mgr / zhc_adapter_shadow_bridge to register a decoded
// attribute against a device.
void device_shadow_process(const ZapDevice* dev,
                           const ZclAttribute* attrs, uint8_t count);

// Called after a successful command send when config.optimistic==true.
void device_shadow_update_optimistic(uint64_t ieee, const char* key,
                                     uint8_t val_type, int32_t int_val);

// Read current state. Returns number of attrs copied into out[0..max_count-1].
uint8_t device_shadow_get_attrs(uint64_t ieee,
                                ShadowAttr* out, uint8_t max_count);

// Read ONE attribute by key. Returns true and copies the matching slot into
// *out when the device exists and holds an attr whose key equals `key`;
// false otherwise. Lets callers avoid pulling the whole 32-slot array (~2.7
// KB) onto the stack just to read a single attr.
//
// Lock contract (T10): takes ONLY the leaf s_mutex — no NVS, no
// event_bus_publish, no other lock acquired inside. Safe to call from the
// event-drain task. `out` must be non-null.
bool device_shadow_get_attr(uint64_t ieee, const char* key, ShadowAttr* out);

// Per-device config CRUD.
bool device_shadow_set_config(uint64_t ieee, const DeviceConfig* cfg);
bool device_shadow_get_config(uint64_t ieee, DeviceConfig* out);

// Clear cached attr state for a rejoining device.
// The NVS blob is erased so stale data is not reloaded on the next boot.
void device_shadow_clear_attrs(uint64_t ieee);

// Set per-device occupancy timeout. 0 = disabled.
// Returns false if device not found.
bool device_shadow_set_occupancy_timeout(uint64_t ieee, uint16_t timeout_s);

// Set per-device debounce window. 0 = disabled (every attr publishes
// immediately). Non-zero = coalesce repeated attrs by last-write-wins
// within the window. Used as "flood protection" for chatty devices
// (e.g. some Tuya thermostats that spam ~10 msg/s). Persisted to NVS.
bool device_shadow_set_debounce_ms(uint64_t ieee, uint32_t debounce_ms);

// Set per-device report rate-limit (shadow_pipeline_throttle_pass): at most one
// state update per `throttle_ms` window, regardless of value change (unlike
// debounce, which only coalesces identical values). 0 = disabled. For
// flood-prone Tuya-DP sensors (air-quality monitors) that report every few
// seconds with no device-side reporting-interval control. Persisted to NVS.
bool device_shadow_set_throttle_ms(uint64_t ieee, uint32_t throttle_ms);
