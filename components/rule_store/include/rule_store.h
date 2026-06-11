// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "zap_common.h"
#include <stdbool.h>

// Initialise NVS namespace zap_rules. Call once from app_main.
void rule_store_init();

// Save (create or update) a RuleSlot. Returns true on success.
bool rule_store_save(const RuleSlot* slot);

// Load a single RuleSlot by rule_id. Returns false if not found.
bool rule_store_load(uint16_t rule_id, RuleSlot* out);

// Delete a rule. Returns false if not found OR if the erase/commit
// failed (the flush path distinguishes the two via the internal
// rule_store_delete_err tri-state).
bool rule_store_delete(uint16_t rule_id);

// Load all saved RuleSlots into out[0..max_count-1]. Returns count loaded.
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count);

// Number of persisted rule slots in NVS (cheap key-count; does not merge
// the writeback overlay or validate CRCs). Lets a fixed-size-cache reader
// detect that more rules are stored than it can hold active. Returns 0 on
// an uninitialised store.
uint16_t rule_store_count();

// Update only the enabled flag of an existing rule. Returns false if not found.
bool rule_store_set_enabled(uint16_t rule_id, bool enabled);

// Highest rule_id present across the ENTIRE persisted store — all NVS
// slots (up to ZAP_MAX_RULES) plus any uncommitted writeback edits — not
// just the subset another layer happens to have cached. Returns 0 when the
// store is empty. Callers allocating a fresh id MUST derive it from this
// (max_id + 1), never from a partial in-memory view, or a persisted-but-
// uncached rule's id can be reissued and silently overwritten. Cheap: ids
// are parsed straight from the NVS key (`r_%04X`), no blob loads.
uint16_t rule_store_max_id();

// ── Writeback cache (PSRAM-backed, deferred NVS commit) ──────────────────
//
// Same pattern as zap_store_flush: keep rule edits in PSRAM, flush a
// batch to NVS every ~5 s. All rule edits are user-triggered so treated
// as HIGH priority (short flush window). rule_store_load / load_all
// consult the pending writeback table so callers always see the latest
// edit, even before it hits flash.
//
// Trades durability (up to 5 s of edits lost on hard power cut) for
// flash wear reduction + sub-ms commit latency in the REST path.

// Start the background flush task. Idempotent.
void rule_store_flush_init();

// Queue a rule save for deferred NVS commit. Replaces direct
// rule_store_save in call sites that care about flash wear.
void rule_store_mark_dirty(const RuleSlot* slot);

// Queue a rule deletion. Overrides any pending dirty-write for same id.
void rule_store_mark_delete(uint16_t rule_id);

// Synchronous full flush — call from shutdown handlers / OTA handoff.
// Durability barrier: also waits (bounded) for commits already in flight
// on the background flush task, so on return pending edits are on flash;
// entries whose write fails twice remain pending and are logged at error
// level. Blocks; task context only (uses vTaskDelay).
void rule_store_flush_now();
