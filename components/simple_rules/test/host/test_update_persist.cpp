// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regressions for simple_rules_update() persistence (REPORT.md §2.1):
//   #1 — update() must PRESERVE a rule's enabled state, not force enabled=1.
//        Otherwise editing a disabled rule silently re-enables it in NVS and it
//        comes back active on the next reload.
//   #2 — update() of an unknown rule_id must NOT persist an orphan enabled rule;
//        it must fail instead.
#include "simple_rules.h"
#include "rule_store.h"
#include "event_bus.h"

#include <cstdio>
#include <cstring>

extern void     stub_rule_store_reset(void);
extern uint16_t stub_rule_store_count(void);
extern bool     stub_rule_store_last_dirty(RuleSlot* out);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

int main() {
    event_bus_init();

    // ── #1: editing a DISABLED rule must not re-enable it in NVS ───────────
    stub_rule_store_reset();
    simple_rules_init();

    uint16_t id = 0;
    CHECK(simple_rules_add("r1", "ON Event#go DO log x ENDON", &id),
          "add rule");
    CHECK(simple_rules_enable(id, false),
          "disable the rule");
    CHECK(simple_rules_update(id, "r1", "ON Event#go DO log y ENDON"),
          "update of the (disabled) rule succeeds");

    RuleSlot persisted{};
    CHECK(stub_rule_store_last_dirty(&persisted),
          "update persisted a slot");
    CHECK(persisted.enabled == 0,
          "update PRESERVES the disabled state (does not force enabled=1)");

    // ── #2: update() of an unknown rule_id must not create an orphan ───────
    stub_rule_store_reset();
    simple_rules_init();

    uint16_t before = stub_rule_store_count();
    bool ok = simple_rules_update(9999, "ghost", "ON Event#z DO log z ENDON");
    CHECK(!ok, "update of an unknown rule_id fails");
    CHECK(stub_rule_store_count() == before,
          "update of an unknown rule_id persists no orphan");

    return s_failures ? 1 : 0;
}
