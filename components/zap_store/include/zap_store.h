// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include "zap_common.h"

// ── Schema migration & namespace contract ────────────────────────────────
//
// Two independent version axes coexist here:
//
//   1. NVS namespace name — a *string* identifier ("zap_v0" today, kept
//      frozen for backwards compatibility with already-deployed devices).
//      Renaming the namespace orphans the previous keys until the next
//      `nvs_flash_erase()` — DO NOT rename without an explicit migration
//      step that walks the old namespace and rewrites under the new one.
//
//   2. `ZAP_STORE_SCHEMA_VERSION` — an integer stored under key
//      "schema_ver" inside the namespace. Bumped whenever the on-flash
//      `ZapDevice` layout changes. A mismatch at boot calls
//      `nvs_erase_all()` on the namespace and writes the new version
//      (devices re-pair) — this is the salvage-free migration path.
//
// When to do what:
//   - Layout-only change (new field, reordered field, widened field):
//     bump SCHEMA_VERSION, document the version delta in the inline
//     comment chain, and accept that devices will be wiped on first
//     boot of the new firmware. CRC32 catches any read of stale blobs
//     that slip through.
//   - Layout change with salvage (e.g. you can re-derive new fields
//     from old): bump SCHEMA_VERSION and add a migration handler in
//     `zap_store_init()` between the version check and the
//     `nvs_erase_all()` — read old blobs, transform, write back, then
//     update "schema_ver". Skip the erase.
//   - Adding a parallel data store (e.g. groups, scenes): use a NEW
//     NVS namespace ("zap_groups", "zap_scenes"), do NOT extend
//     "zap_v0". Keeping unrelated keys in separate namespaces makes
//     selective wipe and schema evolution independent per dataset.

// Initialise NVS — call once from app_main before any other zap_store calls.
void zap_store_init();

// Returns true if zap_store_init() has been called successfully.
bool zap_store_is_ready();

// Save a single device to NVS. Returns true on success.
bool zap_store_save_device(const ZapDevice* dev);

// Delete a single device from NVS by IEEE address. Returns true if found and removed.
bool zap_store_delete_device(uint64_t ieee);

// Load all saved devices into pool. Returns number loaded.
uint16_t zap_store_load_devices(ZapDevice* pool, uint16_t max_count);

// ── Writeback cache (deferred persistence) ────────────────────────────────
//
// Marks a device as "pending write-back" instead of hitting flash
// immediately. Dramatically reduces flash wear for runtime bookkeeping
// (identity re-match, configure state transitions) while still meeting
// durability for user-visible actions (rename, interview end).
//
// Priority semantics:
//   HIGH  — user-triggered or first-contact state. Flushed within
//           ~5 s by the background flush task.
//   LOW   — runtime-only state (support/configure transitions, late
//           identity). Flushed within ~300 s, or on shutdown/OTA.
//
// The zap_store layer is backend-agnostic — it needs a way to snapshot
// the current in-memory ZapDevice for each dirty IEEE. Callers that
// track device state (e.g. zigbee_mgr) install a snapshot callback at
// init. Without a callback, mark_dirty falls back to an immediate
// write (caller-supplied struct pointer, same as save_device).
typedef enum {
    ZAP_PERSIST_LOW  = 0,
    ZAP_PERSIST_HIGH = 1,
} ZapPersistPriority;

// Callback contract: on success, `*out` is filled with the current
// device state and true is returned. Returning false (device gone)
// causes the dirty entry to be dropped silently.
typedef bool (*ZapStoreSnapshotCb)(uint64_t ieee, ZapDevice* out);

// Install once at startup, before any mark_dirty calls.
void zap_store_set_snapshot_cb(ZapStoreSnapshotCb cb);

// Start the background flush task (1 s tick). Safe to call multiple
// times; second call is a no-op.
void zap_store_flush_init();

// Queue `dev` for deferred write-back with the given priority. If the
// same IEEE is already dirty with a lower priority, the priority is
// upgraded. `dev->ieee_addr` is the key.
void zap_store_mark_dirty(const ZapDevice* dev, ZapPersistPriority pri);

// Synchronously flush every dirty entry to NVS. Call from shutdown
// handlers (esp_register_shutdown_handler), OTA handoff, etc.
// Durability barrier: also waits (bounded) for writes already in flight
// on the background flush task, so on return pending state is on flash
// — not merely dequeued; entries whose write fails twice remain pending
// and are logged at error level.
// Blocks; task context only (uses vTaskDelay).
void zap_store_flush_now();

// Force-flush one device by IEEE (blocking, task context only). Returns
// true once the device's pending state is durable on flash (or nothing
// was pending); false if the NVS write failed or an in-flight write did
// not settle within the bounded wait.
bool zap_store_flush_device(uint64_t ieee);
