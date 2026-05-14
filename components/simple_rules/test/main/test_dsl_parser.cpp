// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "simple_rules.h"
#include <cstring>

// ── Trigger parsing ───────────────────────────────────────────────────────

TEST_CASE("dsl: DEVICE_ATTR trigger device#key=\"str\"", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON kitchen_switch#state=\"on\" DO log x ENDON", 1, &r));
    TEST_ASSERT_EQUAL(TriggerType::DEVICE_ATTR, r.trigger.type);
    TEST_ASSERT_EQUAL_STRING("state", r.trigger.attr_key);
    TEST_ASSERT_EQUAL(CondOp::EQ, r.trigger.op);
    TEST_ASSERT_EQUAL(VAL_STR, r.trigger.match_val_type);
    TEST_ASSERT_EQUAL_STRING("on", r.trigger.str_val);
}

TEST_CASE("dsl: DEVICE_ATTR with > operator", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON temp_sensor#temperature>30 DO log hot ENDON", 2, &r));
    TEST_ASSERT_EQUAL(CondOp::GT, r.trigger.op);
    TEST_ASSERT_EQUAL(VAL_INT, r.trigger.match_val_type);
    // attr_keys-era divisor scaling is gone — literal stored verbatim.
    TEST_ASSERT_EQUAL(30, r.trigger.int_val);
}

TEST_CASE("dsl: DEVICE_ATTR with <= operator", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON temp_sensor#temperature<=25 DO log cool ENDON", 3, &r));
    TEST_ASSERT_EQUAL(CondOp::LTE, r.trigger.op);
    TEST_ASSERT_EQUAL(25, r.trigger.int_val);
}

TEST_CASE("dsl: System#Boot trigger", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO log boot ENDON", 4, &r));
    TEST_ASSERT_EQUAL(TriggerType::BOOT, r.trigger.type);
}

TEST_CASE("dsl: Time#Cron trigger", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON Time#Cron=*/5 * * * * DO log tick ENDON", 5, &r));
    TEST_ASSERT_EQUAL(TriggerType::TIME_CRON, r.trigger.type);
    TEST_ASSERT_EQUAL_STRING("*/5 * * * *", r.trigger.key);
}

TEST_CASE("dsl: Event#name trigger", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON Event#my_event DO log evt ENDON", 6, &r));
    TEST_ASSERT_EQUAL(TriggerType::EVENT, r.trigger.type);
    TEST_ASSERT_EQUAL_STRING("my_event", r.trigger.key);
}

TEST_CASE("dsl: Rules#Timer=3 trigger", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON Rules#Timer=3 DO log timer ENDON", 7, &r));
    TEST_ASSERT_EQUAL(TriggerType::TIMER, r.trigger.type);
    TEST_ASSERT_EQUAL_STRING("3", r.trigger.key);
}

TEST_CASE("dsl: IEEE address in trigger", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON 0xAABBCCDDEEFF0011#state=\"on\" DO log x ENDON", 8, &r));
    TEST_ASSERT_EQUAL(TriggerType::DEVICE_ATTR, r.trigger.type);
    TEST_ASSERT_EQUAL(0xAABBCCDDEEFF0011ULL, r.trigger.ieee);
    TEST_ASSERT_EQUAL_STRING("state", r.trigger.attr_key);
}

// ── Action parsing ────────────────────────────────────────────────────────

TEST_CASE("dsl: zigbee.set action", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO zigbee.set socket_b state off ENDON", 10, &r));
    TEST_ASSERT_EQUAL(1, r.action_count);
    TEST_ASSERT_EQUAL(ActionType::ZIGBEE_SET, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("socket_b", r.actions[0].arg0);
    TEST_ASSERT_EQUAL_STRING("state",    r.actions[0].arg1);
    TEST_ASSERT_EQUAL_STRING("off",      r.actions[0].arg2);
}

TEST_CASE("dsl: publish action", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO publish home/alert Motion ENDON", 11, &r));
    TEST_ASSERT_EQUAL(ActionType::PUBLISH, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("home/alert", r.actions[0].arg0);
    TEST_ASSERT_EQUAL_STRING("Motion",     r.actions[0].arg1);
}

TEST_CASE("dsl: event action", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO event my_event ENDON", 12, &r));
    TEST_ASSERT_EQUAL(ActionType::EVENT, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("my_event", r.actions[0].arg0);
}

TEST_CASE("dsl: timer action", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO timer 3 5000 ENDON", 13, &r));
    TEST_ASSERT_EQUAL(ActionType::TIMER, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("3",    r.actions[0].arg0);
    TEST_ASSERT_EQUAL_STRING("5000", r.actions[0].arg1);
}

TEST_CASE("dsl: log action", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO log hello ENDON", 14, &r));
    TEST_ASSERT_EQUAL(ActionType::LOG, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("hello", r.actions[0].arg0);
}

// Regression: the substring fed to parse_action is sliced between
// `DO ` and `ENDON`, so the last action's last arg carries the space
// before ENDON. With strncpy, arg1 became "state " — and the runtime
// then logged `attr 'state ' on '<dev>' missing` because the shadow
// lookup keyed on the literal "state " (trailing space) never hit.
TEST_CASE("dsl: zigbee.toggle action — no trailing space leak", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON sw#action=single DO zigbee.toggle tuya_socket state ENDON", 15, &r));
    TEST_ASSERT_EQUAL(ActionType::ZIGBEE_TOGGLE, r.actions[0].type);
    TEST_ASSERT_EQUAL_STRING("tuya_socket", r.actions[0].arg0);
    TEST_ASSERT_EQUAL_STRING("state",       r.actions[0].arg1);
}

TEST_CASE("dsl: zigbee.set tail arg2 — no trailing space leak", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO zigbee.set kitchen state 1 ENDON", 16, &r));
    TEST_ASSERT_EQUAL_STRING("1", r.actions[0].arg2);
}

// ── Multi-action ──────────────────────────────────────────────────────────

TEST_CASE("dsl: two actions separated by semicolon", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO zigbee.set s state off; log done ENDON", 20, &r));
    TEST_ASSERT_EQUAL(2, r.action_count);
    TEST_ASSERT_EQUAL(ActionType::ZIGBEE_SET, r.actions[0].type);
    TEST_ASSERT_EQUAL(ActionType::LOG,        r.actions[1].type);
}

TEST_CASE("dsl: four actions (max)", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO log a; log b; log c; log d ENDON", 21, &r));
    TEST_ASSERT_EQUAL(4, r.action_count);
}

// ── Variable tokens ───────────────────────────────────────────────────────

TEST_CASE("dsl: %value% passes through to IR arg", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::OK,
        dsl_parse("ON temp#temperature>30 DO publish home/alert %value% ENDON", 30, &r));
    TEST_ASSERT_EQUAL_STRING("%value%", r.actions[0].arg1);
}

// ── Error cases ───────────────────────────────────────────────────────────

TEST_CASE("dsl: missing ENDON returns ERR_NO_ENDON", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_NOT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO log x", 40, &r));
}

TEST_CASE("dsl: missing ON returns ERR_NO_ON", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::ERR_NO_ON,
        dsl_parse("System#Boot DO log x ENDON", 41, &r));
}

TEST_CASE("dsl: missing DO returns ERR_NO_DO", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_EQUAL(ParseResult::ERR_NO_DO,
        dsl_parse("ON System#Boot ENDON", 43, &r));
}

TEST_CASE("dsl: too many actions returns ERR_TOO_MANY_ACTIONS", "[dsl_parser]") {
    ParsedRule r{};
    TEST_ASSERT_NOT_EQUAL(ParseResult::OK,
        dsl_parse("ON System#Boot DO log a; log b; log c; log d; log e ENDON", 42, &r));
}

TEST_CASE("dsl: invalid DSL does not crash", "[dsl_parser]") {
    ParsedRule r{};
    ParseResult res = dsl_parse("garbage input !!!!", 50, &r);
    TEST_ASSERT_NOT_EQUAL(ParseResult::OK, res);
}
