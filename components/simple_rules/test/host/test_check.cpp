// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// rule.check (spec 2026-10-05 §3.3): simple_rules_check parses like a save and stores nothing. The parser
// names field errors but leaves structural ones (no ON / DO / ENDON) unnamed and keeps the previous
// message, so a check could answer a stale reason; the check names them.
#include "simple_rules.h"
#include "rule_store.h"
#include "event_bus.h"

#include <cstdio>
#include <cstring>
#include <string>

extern void     stub_rule_store_reset(void);
extern uint16_t stub_rule_store_count(void);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static int s_changed = 0;

int main() {
    event_bus_init();
    stub_rule_store_reset();
    simple_rules_init();
    event_bus_subscribe(EventType::RULE_CHANGED, [](const Event&) { s_changed++; });

    char err[96];
    CHECK(simple_rules_check("ON Event#go DO log x ENDON", err, sizeof err) && err[0] == '\0',
          "a valid rule passes, with no message");

    CHECK(!simple_rules_check("ON dev#temperature>99999999999.5 DO log x ENDON", err, sizeof err)
              && std::strstr(err, "out of range") != nullptr,
          "a field error comes back in the parser's own words");

    // Right after a failure with a message: a structural error must not repeat "out of range".
    CHECK(!simple_rules_check("ON Event#go DO log x", err, sizeof err) && std::strcmp(err, "missing ENDON") == 0,
          "a missing ENDON is named, not the previous error");
    CHECK(!simple_rules_check("log x ENDON", err, sizeof err) && std::strcmp(err, "rule must start with ON") == 0,
          "a missing ON is named");
    CHECK(!simple_rules_check("ON Event#go log x ENDON", err, sizeof err) && std::strcmp(err, "missing DO") == 0,
          "a missing DO is named");
    CHECK(!simple_rules_check("ON rrs#a=x\"y DO log x ENDON", err, sizeof err)
              && std::strcmp(err, "invalid numeric literal 'x\"y'") == 0,
          "a quote in the user's literal comes back as it was typed");

    std::string pad = "ON Event#go DO log x ENDON" + std::string(473, ' ');   // 499 bytes
    CHECK(pad.size() == 499 && simple_rules_check(pad.c_str(), err, sizeof err), "499 bytes is fine");
    pad += ' ';                                                                 // 500 bytes
    CHECK(!simple_rules_check(pad.c_str(), err, sizeof err)
              && std::strcmp(err, "rule DSL too long (max 499 bytes)") == 0,
          "500 bytes is refused before parsing");

    CHECK(!simple_rules_check(nullptr, err, sizeof err) && std::strcmp(err, "rule must start with ON") == 0,
          "no text at all is a parse failure, not a crash");

    event_bus_drain(EventType::RULE_CHANGED, 0);
    CHECK(s_changed == 0, "a check publishes no RULE_CHANGED");
    CHECK(stub_rule_store_count() == 0, "a check stores nothing");

    uint16_t id = 0;
    CHECK(!simple_rules_add("r", "ON Event#go DO log x", &id) && std::strcmp(dsl_last_error(), "missing ENDON") == 0,
          "rule.create names a structural error too");
    CHECK(!simple_rules_update(9999, "r", "ON Event#go DO log x ENDON") && std::strcmp(dsl_last_error(), "rule not found") == 0,
          "rule.update of an unknown id says so");
    return s_failures ? 1 : 0;
}
