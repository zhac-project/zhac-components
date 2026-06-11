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

// Delete a rule. Returns false if not found.
bool rule_store_delete(uint16_t rule_id);

// Load all saved RuleSlots into out[0..max_count-1]. Returns count loaded.
uint16_t rule_store_load_all(RuleSlot* out, uint16_t max_count);

// Update only the enabled flag of an existing rule. Returns false if not found.
bool rule_store_set_enabled(uint16_t rule_id, bool enabled);

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
// on the background flush task, so on return pending edits are on flash.
// Blocks; task context only (uses vTaskDelay).
void rule_store_flush_now();
