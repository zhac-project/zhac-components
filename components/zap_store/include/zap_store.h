// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include "zap_common.h"

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
void zap_store_flush_now();

// Force-flush one device by IEEE (blocking). Returns true if the entry
// was flushed or not present.
bool zap_store_flush_device(uint64_t ieee);
