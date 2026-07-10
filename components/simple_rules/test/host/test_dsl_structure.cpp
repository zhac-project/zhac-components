// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Structural coverage of dsl_parse — the `ON <trigger> DO <actions> ENDON`
// grammar and its field limits, complementing test_integrity (which owns the
// numeric-literal edges, the action-section overflow, and id/capacity):
//   • every top-level ParseResult path (NO_ON / NO_DO / NO_ENDON / BAD_TRIGGER
//     / OK) incl. the "ON " and " DO " spacing rules and leading-ws tolerance;
//   • field-length limits — attr_key (max 27), string value (max 47), trigger
//     secondary key (max 19) reject on overflow; action args truncate;
//   • trigger-type structural parse (DEVICE_ATTR / System#Boot / Time#Cron= /
//     Event# / Rules#Timer= / Mqtt#) inspected on the public ParsedRule;
//   • friendly-name → IEEE resolution (resolved name filters by device; an
//     unresolved name stays a wildcard).
#include "simple_rules.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

extern void        stub_pool_seed(const ZapDevice* dev);
extern void        stub_shadow_opt_reset(void);
extern int         stub_shadow_opt_count(void);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kIeee  = 0xAABBCCDDEEFF0001ULL;
static constexpr uint64_t kOther = 0x1122334455667788ULL;

static ParseResult pr(const char* dsl) {
    ParsedRule r{};
    return dsl_parse(dsl, 1, &r);
}

static void publish_attr_ieee(uint64_t ieee, const char* key, uint8_t vt, int32_t v) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = ieee;
    ze.val_type = vt;
    std::strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    ze.key[ATTR_KEY_MAX - 1] = '\0';
    ze.int_val = v;
    event_bus_publish(ev);
}
static void drain_attr() { for (int i = 0; i < 8; i++) if (event_bus_drain(EventType::ZCL_ATTR, 0) == 0) break; }

static uint16_t add(const char* name, const char* dsl) {
    uint16_t id = 0;
    if (!simple_rules_add(name, dsl, &id)) return 0;
    return id;
}

int main() {
    // ── A. Top-level structure / ParseResult paths ───────────────────────
    CHECK(pr("ON x#a=1 DO log m ENDON") == ParseResult::OK, "well-formed rule parses OK");
    CHECK(pr("   ON x#a=1 DO log m ENDON") == ParseResult::OK, "leading whitespace tolerated");
    CHECK(pr("log m ENDON")        == ParseResult::ERR_NO_ON,  "missing ON → ERR_NO_ON");
    CHECK(pr("OFF x DO log m ENDON") == ParseResult::ERR_NO_ON, "wrong keyword → ERR_NO_ON");
    CHECK(pr("ONx DO log m ENDON")  == ParseResult::ERR_NO_ON,  "ON without trailing space → ERR_NO_ON");
    CHECK(pr("")                    == ParseResult::ERR_NO_ON,  "empty string → ERR_NO_ON");
    CHECK(pr("ON x#a=1 log m ENDON") == ParseResult::ERR_NO_DO, "missing DO → ERR_NO_DO");
    CHECK(pr("ON x#a=1 DOlog m ENDON") == ParseResult::ERR_NO_DO, "DO without trailing space → ERR_NO_DO");
    CHECK(pr("ON x#a=1 DO log m")    == ParseResult::ERR_NO_ENDON, "missing ENDON → ERR_NO_ENDON");
    {
        std::string big = "ON " + std::string(130, 'd') + "#a=1 DO log m ENDON";
        CHECK(pr(big.c_str()) == ParseResult::ERR_BAD_TRIGGER, "trigger ≥128 bytes → ERR_BAD_TRIGGER");
    }

    // ── B. Field-length limits ───────────────────────────────────────────
    CHECK(pr(("ON dev#" + std::string(27, 'k') + "=1 DO log m ENDON").c_str()) == ParseResult::OK,
          "attr_key of 27 chars accepted");
    CHECK(pr(("ON dev#" + std::string(28, 'k') + "=1 DO log m ENDON").c_str()) != ParseResult::OK,
          "attr_key of 28 chars (>max 27) rejected");
    CHECK(pr(("ON dev#action=\"" + std::string(47, 's') + "\" DO log m ENDON").c_str()) == ParseResult::OK,
          "string value of 47 chars accepted");
    CHECK(pr(("ON dev#action=\"" + std::string(48, 's') + "\" DO log m ENDON").c_str()) != ParseResult::OK,
          "string value of 48 chars (>max 47) rejected");
    CHECK(pr(("ON Event#" + std::string(19, 'e') + " DO log m ENDON").c_str()) == ParseResult::OK,
          "trigger key of 19 chars accepted");
    CHECK(pr(("ON Event#" + std::string(20, 'e') + " DO log m ENDON").c_str()) != ParseResult::OK,
          "trigger key of 20 chars (>max 19) rejected");
    {
        // Action arg0 is 32 B: copy_token truncates a longer device name to 31.
        ParsedRule r{};
        std::string dsl = "ON x#a=1 DO zigbee.set " + std::string(40, 'A') + " state 1 ENDON";
        CHECK(dsl_parse(dsl.c_str(), 1, &r) == ParseResult::OK && std::strlen(r.actions[0].arg0) == 31u,
              "over-long action arg is truncated to the buffer (31 chars), not rejected");
    }

    // ── C. Trigger-type structural parse (inspect the public ParsedRule) ─
    {
        ParsedRule r{};
        CHECK(dsl_parse("ON lamp#state=1 DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::DEVICE_ATTR &&
              strcmp(r.trigger.attr_key, "state") == 0 &&
              strcmp(r.trigger.device_name, "lamp") == 0 &&
              r.trigger.op == CondOp::EQ && r.trigger.int_val == 1,
              "DEVICE_ATTR: attr_key + device_name + op + value populated");

        CHECK(dsl_parse("ON System#Boot DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::BOOT, "System#Boot → TriggerType::BOOT");

        CHECK(dsl_parse("ON Time#Cron=0 0 * * * DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::TIME_CRON &&
              strcmp(r.trigger.key, "0 0 * * *") == 0,
              "Time#Cron= → TIME_CRON with the expression as key");

        CHECK(dsl_parse("ON Event#dusk DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::EVENT && strcmp(r.trigger.key, "dusk") == 0,
              "Event#<name> → EVENT with name as key");

        CHECK(dsl_parse("ON Rules#Timer=3 DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::TIMER && strcmp(r.trigger.key, "3") == 0,
              "Rules#Timer=<n> → TIMER with index as key");

        CHECK(dsl_parse("ON Mqtt#home/sun DO log m ENDON", 1, &r) == ParseResult::OK &&
              r.trigger.type == TriggerType::MQTT_TOPIC && strcmp(r.trigger.key, "home/sun") == 0,
              "Mqtt#<topic> → MQTT_TOPIC with topic as key");
    }

    // ── D. Friendly-name → IEEE resolution (behavioural) ─────────────────
    event_bus_init();
    simple_rules_init();
    ZapDevice dev{};
    dev.ieee_addr = kIeee;
    dev.nwk_addr  = 0x1274;
    dev.endpoints[0] = 1; dev.endpoint_count = 1;
    std::strncpy(dev.friendly_name, "lamp", sizeof(dev.friendly_name) - 1);
    std::strncpy(dev.model_id, "TS0503B", sizeof(dev.model_id) - 1);
    stub_pool_seed(&dev);

    {
        uint16_t id = add("d1", "ON lamp#state=1 DO zigbee.set lamp brightness 9 ENDON");
        stub_shadow_opt_reset();
        publish_attr_ieee(kIeee, "state", VAL_INT, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 1, "resolved friendly name: matching IEEE fires");
        stub_shadow_opt_reset();
        publish_attr_ieee(kOther, "state", VAL_INT, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 0, "resolved friendly name: a different IEEE is filtered");
        simple_rules_delete(id);

        // CODEX H-03: a name not in the pool stays unresolved → INERT. It must
        // NOT match every device — otherwise a typo'd/renamed/unpaired device
        // name silently actuates an unrelated device.
        id = add("d2", "ON ghostdev#state=1 DO zigbee.set lamp brightness 9 ENDON");
        stub_shadow_opt_reset();
        publish_attr_ieee(kOther, "state", VAL_INT, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 0, "unresolved friendly name is inert (not a wildcard)");
        simple_rules_delete(id);
    }

    // ── E. Tier-2 value expressions (parse level) ─────────────────────────
    {
        ParsedRule r{};
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp state !%value% ENDON", 1, &r) == ParseResult::OK &&
              r.actions[0].has_expr,
              "!%value% compiles as a value expression");
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp brightness (%value%*10)/3+5 ENDON", 1, &r) == ParseResult::OK &&
              r.actions[0].has_expr,
              "parenthesised expression accepted");
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp brightness ( %value% * 10 ) / 3 + 5 ENDON", 1, &r) == ParseResult::OK &&
              r.actions[0].has_expr,
              "expression with spaces spans the whole value tail");
        // Compat: literal + bare passthrough stay on the legacy path.
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp state 1 ENDON", 1, &r) == ParseResult::OK &&
              !r.actions[0].has_expr && strcmp(r.actions[0].arg2, "1") == 0,
              "literal value unchanged (no expression)");
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp state %value% ENDON", 1, &r) == ParseResult::OK &&
              !r.actions[0].has_expr && strcmp(r.actions[0].arg2, "%value%") == 0,
              "bare %value% passthrough unchanged");
        // publish payload gets the same treatment.
        CHECK(dsl_parse("ON x#a=1 DO publish home/x %value%*2 ENDON", 1, &r) == ParseResult::OK &&
              r.actions[0].has_expr,
              "publish payload expression compiles");
        CHECK(dsl_parse("ON x#a=1 DO publish home/x hello ENDON", 1, &r) == ParseResult::OK &&
              !r.actions[0].has_expr && strcmp(r.actions[0].arg1, "hello") == 0,
              "publish literal payload unchanged");
        // Rejects surface at rule-add.
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp state %value%/0 ENDON", 1, &r) != ParseResult::OK,
              "literal /0 expression rejected at add");
        CHECK(dsl_parse("ON x#a=1 DO zigbee.set lamp state %value%) ENDON", 1, &r) != ParseResult::OK,
              "malformed expression rejected at add");
        {
            std::string big = "ON x#a=1 DO zigbee.set lamp state %value%+" +
                              std::string(48, '1') + " ENDON";
            CHECK(dsl_parse(big.c_str(), 1, &r) != ParseResult::OK,
                  "over-long value expression rejected at add");
        }
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
