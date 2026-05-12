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

extern ZapDevice* s_pool;
extern uint16_t   s_pool_count;

// Allocate pool in PSRAM — call once before any other pool function.
void zigbee_pool_init();

// Advisory lock — held across multiple pool calls that must be atomic.
void zigbee_pool_lock();
void zigbee_pool_unlock();

// O(1) hash-indexed lookups (open addressing, linear probing).
ZapDevice* pool_find_by_ieee(uint64_t ieee);
ZapDevice* pool_find_by_nwk(uint16_t nwk);

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
inline ZapDevice* pool_all()   { return s_pool; }
inline uint16_t   pool_count() { return s_pool_count; }
