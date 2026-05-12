// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "simple_rules.h"
#include "event_bus.h"
#include <cstring>
#include <cstdint>
#include <cstdlib>

// ── Helpers ───────────────────────────────────────────────────────────────

static ParsedRule make_rule_boot(uint16_t id) {
    ParsedRule r{};
    r.rule_id  = id;
    r.enabled  = true;
    r.trigger.type = TriggerType::BOOT;
    return r;
}

static void set_attr_key(RuleTrigger& t, const char* key) {
    strncpy(t.attr_key, key, ATTR_KEY_MAX - 1);
    t.attr_key[ATTR_KEY_MAX - 1] = '\0';
}

static void set_trigger_str_val(RuleTrigger& t, const char* s) {
    strncpy(t.str_val, s, ATTR_STR_MAX - 1);
    t.str_val[ATTR_STR_MAX - 1] = '\0';
}

static ParsedRule make_rule_device_attr(uint16_t id, uint64_t ieee,
                                        const char* key, CondOp op,
                                        const char* val,
                                        uint8_t vt = VAL_INT) {
    ParsedRule r{};
    r.rule_id                 = id;
    r.enabled                 = true;
    r.trigger.type            = TriggerType::DEVICE_ATTR;
    r.trigger.ieee            = ieee;
    r.trigger.op              = op;
    set_attr_key(r.trigger, key);
    r.trigger.match_val_type  = vt;
    strncpy(r.trigger.value, val, sizeof(r.trigger.value) - 1);
    if (vt == VAL_STR) {
        set_trigger_str_val(r.trigger, val);
        r.trigger.int_val = 0;
    } else if (val && val[0] != '\0') {
        r.trigger.int_val = (int32_t)strtol(val, nullptr, 10);
        r.trigger.str_val[0] = '\0';
    }
    return r;
}

static ParsedRule make_rule_event(uint16_t id, const char* name) {
    ParsedRule r{};
    r.rule_id      = id;
    r.enabled      = true;
    r.trigger.type = TriggerType::EVENT;
    strncpy(r.trigger.key, name, sizeof(r.trigger.key) - 1);
    return r;
}

static ParsedRule make_rule_timer(uint16_t id, int timer_idx) {
    ParsedRule r{};
    r.rule_id      = id;
    r.enabled      = true;
    r.trigger.type = TriggerType::TIMER;
    snprintf(r.trigger.key, sizeof(r.trigger.key), "%d", timer_idx);
    return r;
}

static Event make_zcl_attr(uint64_t ieee, const char* key,
                            int32_t int_val, uint8_t vt = VAL_INT,
                            const char* str_val = nullptr) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee     = ieee;
    ze.val_type = vt;
    strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    ze.key[ATTR_KEY_MAX - 1] = '\0';
    if (vt == VAL_STR && str_val) {
        strncpy(ze.str_val, str_val, ATTR_STR_MAX - 1);
        ze.str_val[ATTR_STR_MAX - 1] = '\0';
    } else {
        ze.int_val = int_val;
    }
    return ev;
}

// ── BOOT trigger ──────────────────────────────────────────────────────────

TEST_CASE("match: BOOT trigger matches CTRL_BOOT event", "[matcher]") {
    ParsedRule r = make_rule_boot(1);
    Event ev{};
    ev.type = EventType::CTRL_BOOT;
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: BOOT trigger does not match ZCL_ATTR", "[matcher]") {
    ParsedRule r = make_rule_boot(2);
    Event ev = make_zcl_attr(0x1122334455667788ULL, "state", 1, VAL_BOOL);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

// ── DEVICE_ATTR trigger ───────────────────────────────────────────────────

TEST_CASE("match: DEVICE_ATTR ANY op matches any int value", "[matcher]") {
    ParsedRule r = make_rule_device_attr(3, 0, "temperature", CondOp::ANY, "");
    Event ev = make_zcl_attr(0xAABBCCDDEEFF0011ULL, "temperature", 25);
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR ieee filter rejects wrong device", "[matcher]") {
    ParsedRule r = make_rule_device_attr(4, 0x1111111111111111ULL, "state", CondOp::ANY, "");
    Event ev = make_zcl_attr(0x2222222222222222ULL, "state", 1, VAL_BOOL);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR ieee=0 matches any device", "[matcher]") {
    ParsedRule r = make_rule_device_attr(5, 0, "state", CondOp::ANY, "");
    Event ev = make_zcl_attr(0xDEADBEEFDEADBEEFULL, "state", 1, VAL_BOOL);
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR key mismatch returns false", "[matcher]") {
    ParsedRule r = make_rule_device_attr(6, 0, "temperature", CondOp::ANY, "");
    Event ev = make_zcl_attr(0, "humidity", 60);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR EQ int matches correct value", "[matcher]") {
    ParsedRule r = make_rule_device_attr(7, 0, "temperature", CondOp::EQ, "30");
    Event ev = make_zcl_attr(0, "temperature", 30);
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("30", val);
}

TEST_CASE("match: DEVICE_ATTR EQ int rejects wrong value", "[matcher]") {
    ParsedRule r = make_rule_device_attr(8, 0, "temperature", CondOp::EQ, "30");
    Event ev = make_zcl_attr(0, "temperature", 20);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR GT operator", "[matcher]") {
    ParsedRule r = make_rule_device_attr(9, 0, "temperature", CondOp::GT, "25");
    Event ev_hi = make_zcl_attr(0, "temperature", 30);
    Event ev_lo = make_zcl_attr(0, "temperature", 20);
    char val[32];
    TEST_ASSERT_TRUE (simple_rules_match(r, ev_hi, val, sizeof(val)));
    TEST_ASSERT_FALSE(simple_rules_match(r, ev_lo, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR LTE operator", "[matcher]") {
    ParsedRule r = make_rule_device_attr(10, 0, "temperature", CondOp::LTE, "25");
    Event ev_eq  = make_zcl_attr(0, "temperature", 25);
    Event ev_lo  = make_zcl_attr(0, "temperature", 10);
    Event ev_hi  = make_zcl_attr(0, "temperature", 26);
    char val[32];
    TEST_ASSERT_TRUE (simple_rules_match(r, ev_eq,  val, sizeof(val)));
    TEST_ASSERT_TRUE (simple_rules_match(r, ev_lo,  val, sizeof(val)));
    TEST_ASSERT_FALSE(simple_rules_match(r, ev_hi,  val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR NEQ operator", "[matcher]") {
    ParsedRule r = make_rule_device_attr(11, 0, "state", CondOp::NEQ, "0", VAL_BOOL);
    Event ev_ne = make_zcl_attr(0, "state", 1, VAL_BOOL);
    Event ev_eq = make_zcl_attr(0, "state", 0, VAL_BOOL);
    char val[32];
    TEST_ASSERT_TRUE (simple_rules_match(r, ev_ne, val, sizeof(val)));
    TEST_ASSERT_FALSE(simple_rules_match(r, ev_eq, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR EQ int match for action key", "[matcher]") {
    ParsedRule r = make_rule_device_attr(12, 0, "action_code", CondOp::EQ, "2");
    Event ev = make_zcl_attr(0, "action_code", 2);
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("2", val);
}

TEST_CASE("match: DEVICE_ATTR EQ int mismatch for action key", "[matcher]") {
    ParsedRule r = make_rule_device_attr(13, 0, "action_code", CondOp::EQ, "2");
    Event ev = make_zcl_attr(0, "action_code", 1);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

// ── String-valued attrs (post-attr_keys: no ID lookup) ────────────────────

TEST_CASE("match: DEVICE_ATTR EQ str matches string value", "[matcher]") {
    ParsedRule r = make_rule_device_attr(30, 0, "action", CondOp::EQ, "single", VAL_STR);
    Event ev = make_zcl_attr(0, "action", 0, VAL_STR, "single");
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("single", val);
}

TEST_CASE("match: DEVICE_ATTR EQ str rejects different string", "[matcher]") {
    ParsedRule r = make_rule_device_attr(31, 0, "action", CondOp::EQ, "single", VAL_STR);
    Event ev = make_zcl_attr(0, "action", 0, VAL_STR, "double");
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: DEVICE_ATTR NEQ str", "[matcher]") {
    ParsedRule r = make_rule_device_attr(32, 0, "action", CondOp::NEQ, "release", VAL_STR);
    Event ev_ne = make_zcl_attr(0, "action", 0, VAL_STR, "hold");
    Event ev_eq = make_zcl_attr(0, "action", 0, VAL_STR, "release");
    char val[32];
    TEST_ASSERT_TRUE (simple_rules_match(r, ev_ne, val, sizeof(val)));
    TEST_ASSERT_FALSE(simple_rules_match(r, ev_eq, val, sizeof(val)));
}

// ── EVENT trigger ─────────────────────────────────────────────────────────

TEST_CASE("match: EVENT trigger matches RULE_EVENT by name", "[matcher]") {
    ParsedRule r = make_rule_event(14, "my_event");
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto& p = *reinterpret_cast<RuleEventPayload*>(ev.data);
    strncpy(p.name, "my_event", sizeof(p.name) - 1);
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: EVENT trigger rejects different name", "[matcher]") {
    ParsedRule r = make_rule_event(15, "my_event");
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto& p = *reinterpret_cast<RuleEventPayload*>(ev.data);
    strncpy(p.name, "other_event", sizeof(p.name) - 1);
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

// ── TIMER trigger ─────────────────────────────────────────────────────────

TEST_CASE("match: TIMER trigger matches correct index", "[matcher]") {
    ParsedRule r = make_rule_timer(16, 3);
    Event ev{};
    ev.type = EventType::RULE_TIMER_FIRE;
    reinterpret_cast<RuleTimerPayload*>(ev.data)->timer_index = 3;
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

TEST_CASE("match: TIMER trigger rejects different index", "[matcher]") {
    ParsedRule r = make_rule_timer(17, 3);
    Event ev{};
    ev.type = EventType::RULE_TIMER_FIRE;
    reinterpret_cast<RuleTimerPayload*>(ev.data)->timer_index = 5;
    char val[32];
    TEST_ASSERT_FALSE(simple_rules_match(r, ev, val, sizeof(val)));
}

// ── Disabled rule ─────────────────────────────────────────────────────────

TEST_CASE("match: disabled rule still matches (caller guards enabled)", "[matcher]") {
    // simple_rules_match itself doesn't check enabled — dispatch does.
    ParsedRule r = make_rule_boot(18);
    r.enabled = false;
    Event ev{};
    ev.type = EventType::CTRL_BOOT;
    char val[32];
    TEST_ASSERT_TRUE(simple_rules_match(r, ev, val, sizeof(val)));
}

// ── event_val fill ────────────────────────────────────────────────────────

TEST_CASE("match: event_val is empty string for BOOT trigger", "[matcher]") {
    ParsedRule r = make_rule_boot(19);
    Event ev{};
    ev.type = EventType::CTRL_BOOT;
    char val[32] = "previous";
    simple_rules_match(r, ev, val, sizeof(val));
    TEST_ASSERT_EQUAL_STRING("", val);
}

TEST_CASE("match: event_val filled with int as string for ZCL_ATTR", "[matcher]") {
    ParsedRule r = make_rule_device_attr(20, 0, "temperature", CondOp::ANY, "");
    Event ev = make_zcl_attr(0, "temperature", 42);
    char val[32];
    simple_rules_match(r, ev, val, sizeof(val));
    TEST_ASSERT_EQUAL_STRING("42", val);
}

TEST_CASE("match: event_val filled with str for STR-typed ZCL_ATTR", "[matcher]") {
    ParsedRule r = make_rule_device_attr(21, 0, "action", CondOp::ANY, "", VAL_STR);
    Event ev = make_zcl_attr(0, "action", 0, VAL_STR, "flip90");
    char val[32];
    simple_rules_match(r, ev, val, sizeof(val));
    TEST_ASSERT_EQUAL_STRING("flip90", val);
}
