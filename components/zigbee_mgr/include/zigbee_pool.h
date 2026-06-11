// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/include/zigbee_pool.h
#pragma once
#include "zap_common.h"
#include "esp_log.h"
#include <cstring>

// ── Concurrency contract ────────────────────────────────────────────────
//
// The pool is shared between at least four tasks: the interview task
// (mutates on join), the ZCL task (reads on attribute report), the REST
// handlers via the backend (both reads and mutations), and the HAP
// dispatcher (reads on GET_DEVICES). Previously these ran without any
// synchronization, allowing partially-written entries, torn counts during
// swap-with-last removal, and pointer invalidation mid-dereference (see
// QWEN §10 / CODEX §3).
//
// Public lookup/add/remove functions now take an internal mutex for the
// duration of the call. For sequences that need to remain atomic across
// multiple calls — for example, "find this device, then read its endpoint
// list without someone else removing it" — use the advisory pair
// `zigbee_pool_lock()` / `zigbee_pool_unlock()` around the block. The
// mutex is recursive, so nested calls through the public API are safe.

// Pool storage (s_pool, s_pool_count) is implementation-private — moved out
// of the public header so contributors can't bypass the concurrency
// contract by indexing directly into the array. Use pool_all() / pool_count()
// (still inline but consuming non-extern internal accessors) for iteration,
// always under zigbee_pool_lock().

// Allocate pool in PSRAM — call once before any other pool function.
void zigbee_pool_init();

// Load every NVS-persisted device into the in-memory pool. Must be
// called AFTER `zigbee_pool_init()` and `zap_store_init()`. Used at
// boot to restore the device snapshot so paired devices keep working
// across coordinator restarts — z2m behaviour, no re-pair required.
// Combined with the ZNP-side NV preservation (CC2652 keeps PAN ID +
// NWK key + child table across resets) this gives full restart parity.
// Returns the count loaded; 0 on first boot or zap_store-not-ready.
uint16_t zigbee_pool_restore_persisted();

// Advisory lock — held across multiple pool calls that must be atomic.
// This takes the SAME internal recursive mutex used by every public pool
// function (find/add/remove/restore), so holding it excludes
// swap-with-last relocation by pool_remove().
void zigbee_pool_lock();
void zigbee_pool_unlock();

// O(1) hash-indexed lookups (open addressing, linear probing).
//
// HAZARD: the returned pointer aims into the live pool array and is only
// valid while zigbee_pool_lock() is held. These functions release the
// internal mutex before returning, so an unguarded caller can have its
// slot retargeted at any time by a concurrent pool_remove() (swap-with-
// last moves the LAST device into the removed slot). Valid patterns:
//   (a) snapshot under the advisory lock —
//         zigbee_pool_lock();
//         ZapDevice copy; bool found = false;
//         if (const ZapDevice* d = pool_find_by_ieee(i)) { copy = *d; found = true; }
//         zigbee_pool_unlock();
//       (house pattern — see zhc_shadow_bridge.cpp), or
//   (b) zigbee_pool_with_device() below for short in-place mutations.
// Never dereference the result after the lock is released.
ZapDevice* pool_find_by_ieee(uint64_t ieee);
ZapDevice* pool_find_by_nwk(uint16_t nwk);

// ── Locked visitor API ──────────────────────────────────────────────
// Runs fn(dev, ctx) under the pool mutex iff the device exists; returns
// false when the device is absent (fn not called). The mutex held is the
// same internal recursive mutex taken by zigbee_pool_lock() and by every
// pool mutator, so pool_remove()'s swap-with-last cannot retarget the
// slot while fn runs — the pointer handed to fn is valid for exactly
// fn's duration. Do NOT stash the pointer; copy what you need into ctx.
//
// fn MUST be short and non-blocking: no radio I/O, no NVS access
// (zap_store_mark_dirty's dirty-table-full fallback writes flash — call
// it AFTER the visitor returns, on a snapshot copied out via ctx; see
// on_zdo_leave_ind), and avoid UART logging where possible.
bool zigbee_pool_with_device(uint64_t ieee,
                             void (*fn)(ZapDevice* dev, void* ctx),
                             void* ctx);
bool zigbee_pool_with_device_by_nwk(uint16_t nwk,
                                    void (*fn)(ZapDevice* dev, void* ctx),
                                    void* ctx);

// Force hash rebuild on the next lookup. Must be called after in-place
// mutation of any field consulted by pool_find_by_nwk (currently
// nwk_addr). Forgetting this causes rejoin fast-paths to drop every
// subsequent frame with "AF_INCOMING from unknown nwk=..." because the
// nwk index still maps the old nwk to the device slot.
void zigbee_pool_mark_dirty();

// Allocate a new pool slot. Returns nullptr if the pool is full.
ZapDevice* pool_add();

// Remove device at pool index i (rare operation — rebuilds hash).
void pool_remove(uint16_t idx);

// Direct accessors. Callers that iterate must hold zigbee_pool_lock().
// Implemented in zigbee_pool.cpp so the underlying storage stays private.
ZapDevice* pool_all();
uint16_t   pool_count();

// Active count — pool_count() minus tombstoned (zap_dev_is_removed)
// entries. Use for any externally-reported number (HEARTBEAT.device_count,
// SYNC_ACK device_count, /api/status info.device_count) so the SPA's
// Info page agrees with the Devices page list (which already filters
// removed entries). pool_count() stays for iteration bounds + internal
// allocation accounting.
uint16_t pool_count_active();
