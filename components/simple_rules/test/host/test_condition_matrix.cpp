// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Comprehensive coverage of the simple_rules condition/trigger matrix:
//   • every comparison operator (=, !=, >, <, >=, <=, ANY) on VAL_INT, with
//     below/equal/above boundary cases;
//   • VAL_FLOAT (stored ×100) and VAL_BOOL folded into the integer domain;
//   • VAL_STR equality (=/!=) and the deliberate rejection of ordered ops on
//     strings;
//   • cross-type guards (int literal vs string attr, and vice-versa);
//   • numeric-literal parsing edges (round-half-away, NaN/inf/overflow reject,
//     negative values);
//   • the ieee device filter and the empty-attr wildcard;
//   • every non-DEVICE_ATTR trigger (System#Boot, Event#, Rules#Timer=,
//     Mqtt#) plus cross-trigger-type rejection.
//
// The matcher (simple_rules_match) and dsl_parse are exercised end-to-end; only
// the value/trigger structs are hand-built. Companion to test_bool_match, which
// pins the VAL_BOOL fix specifically.
#include "simple_rules.h"
#include "event_bus.h"

#include <cstdio>
#include <cstring>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// ── Event builders ─────────────────────────────────────────────────────────
static Event make_attr_event(const char* key, uint8_t val_type,
                             int32_t int_val, const char* str_val,
                             uint64_t ieee = 0xAABBCCDDEEFF0001ULL) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = ieee;
    ze.val_type = val_type;
    std::strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    ze.key[ATTR_KEY_MAX - 1] = '\0';
    if (val_type == VAL_STR) {
        std::strncpy(ze.str_val, str_val ? str_val : "", ATTR_STR_MAX - 1);
        ze.str_val[ATTR_STR_MAX - 1] = '\0';
    } else {
        ze.int_val = int_val;
    }
    return ev;
}

static Event make_boot_event() {
    Event ev{};
    ev.type = EventType::CTRL_BOOT;
    return ev;
}

static Event make_rule_event(const char* name) {
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto& re = *reinterpret_cast<RuleEventPayload*>(ev.data);
    std::strncpy(re.name, name, sizeof(re.name) - 1);
    re.name[sizeof(re.name) - 1] = '\0';
    re.hop = 0;
    return ev;
}

static Event make_timer_event(uint8_t index) {
    Event ev{};
    ev.type = EventType::RULE_TIMER_FIRE;
    reinterpret_cast<RuleTimerPayload*>(ev.data)->timer_index = index;
    return ev;
}

static Event make_mqtt_event(const char* topic, const char* payload) {
    Event ev{};
    ev.type = EventType::MQTT_MSG;
    auto& me = *reinterpret_cast<MqttMsgEvent*>(ev.data);
    std::strncpy(me.topic, topic, sizeof(me.topic) - 1);
    me.topic[sizeof(me.topic) - 1] = '\0';
    std::strncpy(me.payload, payload, sizeof(me.payload) - 1);
    me.payload[sizeof(me.payload) - 1] = '\0';
    return ev;
}

// ── Parse + match helpers ──────────────────────────────────────────────────
static bool parse_and_match(const char* dsl, const Event& ev) {
    ParsedRule rule{};
    if (dsl_parse(dsl, 1, &rule) != ParseResult::OK) {
        printf("  (parse failed for '%s': %s)\n", dsl, dsl_last_error());
        return false;
    }
    char buf[64];
    return simple_rules_match(rule, ev, buf, sizeof buf);
}

static bool parse_ok(const char* dsl) {
    ParsedRule rule{};
    return dsl_parse(dsl, 1, &rule) == ParseResult::OK;
}

// Parse, override the (normally resolver-supplied) trigger IEEE, then match.
static bool parse_ieee_match(const char* dsl, uint64_t ieee, const Event& ev) {
    ParsedRule rule{};
    if (dsl_parse(dsl, 1, &rule) != ParseResult::OK) return false;
    rule.trigger.ieee = ieee;
    char buf[64];
    return simple_rules_match(rule, ev, buf, sizeof buf);
}

int main() {
    // ── 1. Operator matrix on VAL_INT (event level = 50) ─────────────────
    const Event l49 = make_attr_event("level", VAL_INT, 49, nullptr);
    const Event l50 = make_attr_event("level", VAL_INT, 50, nullptr);
    const Event l51 = make_attr_event("level", VAL_INT, 51, nullptr);
    #define M(dsl, ev) parse_and_match("ON dev#" dsl " DO log m ENDON", ev)
    CHECK( M("level=50", l50) && !M("level=50", l49) && !M("level=50", l51), "EQ: only equal matches");
    CHECK( M("level!=50", l49) &&  M("level!=50", l51) && !M("level!=50", l50), "NEQ: matches all but equal");
    CHECK( M("level>50", l51) && !M("level>50", l50) && !M("level>50", l49), "GT: strictly above");
    CHECK( M("level<50", l49) && !M("level<50", l50) && !M("level<50", l51), "LT: strictly below");
    CHECK( M("level>=50", l50) && M("level>=50", l51) && !M("level>=50", l49), "GTE: equal or above");
    CHECK( M("level<=50", l50) && M("level<=50", l49) && !M("level<=50", l51), "LTE: equal or below");

    // ── 2. VAL_FLOAT stored ×100 (temperature 25.50 → int_val 2550) ──────
    const Event t2550 = make_attr_event("temperature", VAL_FLOAT, 2550, nullptr);
    CHECK( M("temperature>2500", t2550) && !M("temperature>2550", t2550), "FLOAT GT ×100");
    CHECK( M("temperature>=2550", t2550) && !M("temperature>2550", t2550), "FLOAT GTE vs GT at boundary");
    CHECK( M("temperature<2600", t2550) && !M("temperature<2550", t2550), "FLOAT LT ×100");
    CHECK( M("temperature=2550", t2550), "FLOAT equality ×100");

    // ── 3. VAL_BOOL folds into the integer domain, ordered ops included ──
    const Event c1 = make_attr_event("contact", VAL_BOOL, 1, nullptr);
    const Event c0 = make_attr_event("contact", VAL_BOOL, 0, nullptr);
    CHECK( M("contact!=0", c1) && !M("contact!=0", c0), "BOOL !=0 tracks true");
    CHECK( M("contact>=1", c1) && !M("contact>=1", c0), "BOOL >=1 tracks true");
    CHECK( M("contact<1", c0) && !M("contact<1", c1),  "BOOL <1 tracks false");

    // ── 4. VAL_STR: =/!= only; ordered ops never match ──────────────────
    const Event single = make_attr_event("action", VAL_STR, 0, "single");
    CHECK( M("action=\"single\"", single) && !M("action=\"double\"", single), "STR equality");
    CHECK( M("action!=\"double\"", single) && !M("action!=\"single\"", single), "STR inequality");
    CHECK(!M("action>\"single\"", single) && !M("action<\"single\"", single), "STR ordered ops never match");

    // ── 5. Cross-type guards ────────────────────────────────────────────
    const Event str50 = make_attr_event("level", VAL_STR, 0, "50");
    const Event int50 = make_attr_event("action", VAL_INT, 50, nullptr);
    CHECK(!M("level=50", str50),     "int literal does not match a string attr");
    CHECK(!M("action=\"50\"", int50), "string literal does not match an int attr");

    // ── 6. Numeric-literal parsing edges ────────────────────────────────
    const Event l3 = make_attr_event("level", VAL_INT, 3, nullptr);
    const Event l2 = make_attr_event("level", VAL_INT, 2, nullptr);
    CHECK( M("level=2.5", l3) && !M("level=2.5", l2), "decimal literal rounds half-away (2.5→3)");
    CHECK( M("level=2.4", l2) && !M("level=2.4", l3), "decimal literal 2.4→2");
    const Event neg = make_attr_event("balance", VAL_INT, -5, nullptr);
    CHECK( M("balance<-3", neg) && M("balance=-5", neg), "negative literals compare");
    CHECK(!parse_ok("ON dev#temp>nan DO log m ENDON"),  "NaN literal rejected at parse");
    CHECK(!parse_ok("ON dev#temp>inf DO log m ENDON"),  "inf literal rejected at parse");
    CHECK(!parse_ok("ON dev#temp>1e20 DO log m ENDON"), "out-of-int32 literal rejected at parse");

    // ── 7. Wildcard is the bare `ON <device>` form (no #attr). A trailing
    //       '#' with an empty attr is REJECTED by the parser — pin the asymmetry.
    CHECK( parse_and_match("ON dev DO log m ENDON", l50) &&
           parse_and_match("ON dev DO log m ENDON", single),
          "bare 'ON dev' (no #attr) = wildcard matches any attribute/value");
    CHECK(!parse_ok("ON dev# DO log m ENDON"),
          "trailing '#' with empty attr is a parse error (not a wildcard)");

    // ── 8. attr_key mismatch ────────────────────────────────────────────
    CHECK(!M("occupancy=1", l50), "attr_key mismatch does not fire");

    // ── 9. IEEE device filter ───────────────────────────────────────────
    const uint64_t A = 0x1111111111111111ULL, B = 0x2222222222222222ULL;
    const Event fromA = make_attr_event("level", VAL_INT, 50, nullptr, A);
    CHECK( parse_ieee_match("ON dev#level=50 DO log m ENDON", A, fromA),
          "matching IEEE + value fires");
    CHECK(!parse_ieee_match("ON dev#level=50 DO log m ENDON", B, fromA),
          "non-matching IEEE is filtered out");

    // ── 10. Non-DEVICE_ATTR triggers ────────────────────────────────────
    CHECK( parse_and_match("ON System#Boot DO log m ENDON", make_boot_event()),
          "System#Boot matches CTRL_BOOT");
    CHECK( parse_and_match("ON Event#dusk DO log m ENDON", make_rule_event("dusk")) &&
          !parse_and_match("ON Event#dusk DO log m ENDON", make_rule_event("dawn")),
          "Event#<name> matches by name");
    CHECK( parse_and_match("ON Rules#Timer=3 DO log m ENDON", make_timer_event(3)) &&
          !parse_and_match("ON Rules#Timer=3 DO log m ENDON", make_timer_event(4)),
          "Rules#Timer=<n> matches by index");
    CHECK( parse_and_match("ON Mqtt#home/sun DO log m ENDON", make_mqtt_event("home/sun", "up")) &&
          !parse_and_match("ON Mqtt#home/sun DO log m ENDON", make_mqtt_event("home/moon", "up")),
          "Mqtt#<topic> matches by topic");

    // MQTT payload is captured into event_val for the action layer.
    {
        ParsedRule rule{};
        CHECK(dsl_parse("ON Mqtt#home/sun DO log m ENDON", 1, &rule) == ParseResult::OK, "mqtt rule parses");
        char buf[64] = {0};
        const Event me = make_mqtt_event("home/sun", "risen");
        CHECK(simple_rules_match(rule, me, buf, sizeof buf) && std::strcmp(buf, "risen") == 0,
              "MQTT payload captured into event_val");
    }

    // ── 11. Cross-trigger-type rejection ────────────────────────────────
    CHECK(!parse_and_match("ON System#Boot DO log m ENDON", l50),
          "DEVICE_ATTR event does not fire a BOOT trigger");
    CHECK(!parse_and_match("ON dev#level=50 DO log m ENDON", make_boot_event()),
          "BOOT event does not fire a DEVICE_ATTR trigger");
    CHECK(!parse_and_match("ON Event#x DO log m ENDON", make_timer_event(1)),
          "TIMER event does not fire an EVENT trigger");

    #undef M
    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
