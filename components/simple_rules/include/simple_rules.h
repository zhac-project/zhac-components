// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "zap_common.h"
#include "rule_store.h"
#include "event_bus.h"
#include "zcl_attribute.h"
#include <cstdint>
#include <stdbool.h>

// ── In-memory IR ──────────────────────────────────────────────────────────

enum class CondOp : uint8_t { ANY, EQ, NEQ, GT, LT, GTE, LTE };

struct RuleTrigger {
    TriggerType type;
    uint64_t    ieee;            // 0 = unresolved / any
    // DEVICE_ATTR: semantic attribute name (e.g. "action", "state"). For
    // non-attr triggers (EVENT/CRON/TIMER/MQTT) this doubles as the
    // event name, cron expression, timer index string, or MQTT topic.
    char        attr_key[ATTR_KEY_MAX];
    char        key[20];         // non-attr secondary key slot (kept for ABI)
    CondOp      op;
    // DEVICE_ATTR comparison value (parsed at DSL time):
    //   match_val_type == VAL_INT/BOOL: int_val holds the raw parsed int.
    //   match_val_type == VAL_STR:       str_val holds the literal string;
    //                                    only EQ/NEQ operators are meaningful.
    uint8_t     match_val_type;  // ValType
    int32_t     int_val;         // parsed int value
    char        str_val[ATTR_STR_MAX]; // parsed string value (STR type)
    char        value[20];       // original DSL literal text
    char        device_name[20]; // friendly name before resolution; empty if ieee literal
};

enum class ActionType : uint8_t {
    ZIGBEE_SET    = 1,
    PUBLISH       = 2,
    EVENT         = 3,
    TIMER         = 4,
    LOG           = 5,
    SCRIPT        = 6,   // DO script.run "<script_name>"
    ZIGBEE_TOGGLE = 7,   // DO zigbee.toggle "<device>" <key>
};

// Payload handed to the script hook when an ActionType::SCRIPT action
// fires. Stringified `value` is always present; the raw fields below it
// are populated from the originating Event and let a script handler
// reconstruct the full trigger context. Fields default to 0 / empty
// string for non-DEVICE_ATTR triggers (BOOT, TIMER, MQTT, EVENT).
struct SimpleRulesScriptEvent {
    const char* key;       // attr name, or "" for non-attr triggers
    const char* value;     // stringified trigger value (same as legacy event_val)
    uint64_t    ieee;      // source device IEEE, 0 when not device-triggered
    uint16_t    cluster;   // ZCL cluster id, 0 for non-attr triggers
    uint16_t    attr_id;   // ZCL attribute id, 0 for non-attr triggers
    uint8_t     val_type;  // ValType enum
    int32_t     int_val;   // raw int (VAL_INT/BOOL)
    const char* str_val;   // raw string (VAL_STR), or empty
};

// Callback invoked by the rule engine when an ActionType::SCRIPT action
// fires. Registered at lua_engine init so simple_rules stays free of a
// scripting-engine dependency. `name` is NUL-terminated.
typedef void (*simple_rules_script_hook_t)(const char* name,
                                            const SimpleRulesScriptEvent& ev);
void simple_rules_set_script_hook(simple_rules_script_hook_t hook);

struct RuleAction {
    ActionType type;
    char       arg0[32];  // device ref / topic / event name / message
    char       arg1[20];  // attr key / payload / timer index
    char       arg2[20];  // attr value
};

struct ParsedRule {
    uint16_t    rule_id;
    bool        enabled;
    RuleTrigger trigger;
    RuleAction  actions[4];
    uint8_t     action_count;
};

// Parse result
enum class ParseResult : uint8_t {
    OK = 0,
    ERR_NO_ON,
    ERR_NO_DO,
    ERR_NO_ENDON,
    ERR_BAD_TRIGGER,
    ERR_BAD_ACTION,
    ERR_TOO_MANY_ACTIONS,
    // P2-T18 def 4 (FINDINGS §7): action section overrun the parse buffer.
    // Previously the buffer was silently clamped to 499 bytes, so a long
    // rule parsed a different (truncated) action set than the one stored —
    // now an explicit error instead of a clamp.
    ERR_ACTION_TOO_LONG,
};

// ── Matcher (also used in tests) ─────────────────────────────────────────
// Returns true if rule's trigger matches the event.
// event_val is filled with the string representation of the event value.
bool simple_rules_match(const ParsedRule& rule, const Event& ev,
                        char* event_val, size_t val_size);

// ── DSL Parser API (internal, also used in tests) ─────────────────────────
// Parse one DSL rule string into a ParsedRule. Does NOT resolve friendly names.
// Returns ParseResult::OK on success; rule is filled in.
ParseResult dsl_parse(const char* dsl, uint16_t rule_id, ParsedRule* out);

// Last human-readable parse error (set by dsl_parse on failure). Empty
// string when the last call succeeded or no call has been made yet.
// Used by HAP RULE_* handlers to propagate specific errors to the UI.
const char* dsl_last_error();

// Overwrite the last-error string from outside the parser, so non-parse
// add/update failures (rule cache full, oversize DSL) reach the UI via the
// same RULE_EXEC_RESULT err field the HAP handlers already read. Pass
// nullptr to clear. Caller must serialise (simple_rules holds its mutex).
void dsl_set_last_error(const char* msg);

// Resolve friendly names → IEEE for all DEVICE_ATTR triggers.
// Call after device pool is populated. Sets trigger.ieee; leaves 0 if not found.
void simple_rules_resolve_names(ParsedRule* rules, uint16_t count);

// ── Public API ────────────────────────────────────────────────────────────
void     simple_rules_init();
void     simple_rules_reload();

// `name` is an optional friendly label ≤ 23 chars; pass nullptr or "" to
// leave blank. Stored verbatim in the slot; never used by parsing.
bool     simple_rules_add(const char* name, const char* dsl,
                           uint16_t* out_rule_id);
bool     simple_rules_update(uint16_t rule_id,
                              const char* name, const char* dsl);
bool     simple_rules_delete(uint16_t rule_id);
bool     simple_rules_enable(uint16_t rule_id, bool enabled);

uint16_t simple_rules_list(RuleSlot* out, uint16_t max_count);

// ── Rule error callback ───────────────────────────────────────────────────
// Called when a stored rule DSL fails to parse during reload (e.g. after
// firmware update changes the DSL grammar). Safe to call from any context.
typedef void (*rules_error_cb_t)(uint16_t rule_id, const char* err_msg);
void simple_rules_set_error_cb(rules_error_cb_t cb);
