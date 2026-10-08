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

    // RULE_CHANGED: every mutation publishes once, whichever door called it
    {
        static uint8_t  kinds[8]; static uint16_t ids[8]; static int n = 0;
        n = 0;
        event_bus_subscribe(EventType::RULE_CHANGED, [](const Event& e) {
            RuleChangedEvent c{}; memcpy(&c, e.data, sizeof(c));
            if (n < 8) { kinds[n] = c.change; ids[n] = c.rule_id; n++; }
        });
        uint16_t rid = 0;
        simple_rules_add("r9", "ON Event#go DO log z ENDON", &rid);
        simple_rules_enable(rid, false);
        simple_rules_update(rid, "r9", "ON Event#go DO log zz ENDON");
        simple_rules_delete(rid);
        event_bus_drain(EventType::RULE_CHANGED, 0);
        CHECK(n == 4 && kinds[0] == RULE_CHANGE_ADDED && kinds[1] == RULE_CHANGE_UPDATED &&
              kinds[2] == RULE_CHANGE_UPDATED && kinds[3] == RULE_CHANGE_DELETED,
              "add / enable / update / delete publish RULE_CHANGED added, updated, updated, deleted");
        CHECK(ids[0] == rid && ids[3] == rid, "RULE_CHANGED carries the rule id");
        CHECK(!simple_rules_enable(9999, true) && n == 4, "a failed mutation publishes nothing");
    }

    // A name longer than the 23 bytes a slot keeps is cut between characters, never inside one: a cut
    // character is invalid UTF-8, and one invalid byte in a WebSocket text frame closes the socket.
    {
        static const char kCyr12[] = "абвгдежзийкл";   // 12 letters, 24 bytes
        static const char kCyr11[] = "абвгдежзийк";    // what fits: 22 bytes
        stub_rule_store_reset();
        simple_rules_init();
        uint16_t rid = 0;
        RuleSlot s{};
        CHECK(simple_rules_add(kCyr12, "ON Event#go DO log x ENDON", &rid) && stub_rule_store_last_dirty(&s) &&
              std::strcmp(s.name, kCyr11) == 0, "add: a 12-letter Cyrillic name is stored as its first 11 letters");
        CHECK(simple_rules_update(rid, kCyr12, "ON Event#go DO log y ENDON") && stub_rule_store_last_dirty(&s) &&
              std::strcmp(s.name, kCyr11) == 0, "update: the same cut, between characters");
        CHECK(simple_rules_add("plain_ascii_name_of_30_bytes!!", "ON Event#go DO log x ENDON", &rid) &&
              stub_rule_store_last_dirty(&s) && std::strcmp(s.name, "plain_ascii_name_of_30_") == 0,
              "an ASCII name keeps its first 23 bytes");
    }
    return s_failures ? 1 : 0;
}
