// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// P2-T18 host regressions for simple_rules / dsl_parser input integrity:
//   def 1  — next_rule_id derives from the WHOLE persisted store, not just
//            the 64-entry in-memory cache (a persisted-but-uncached id must
//            never be reissued).
//   def 4  — an oversize DSL (>= sizeof(RuleSlot::src)) is rejected at
//            add/update, never truncate-persisted; action-section overrun
//            yields ERR_ACTION_TOO_LONG instead of a silent clamp.
//   def 5  — numeric literals out of int32 range (1e20 / -1e20 / garbage)
//            return DSL_ERR_BAD_NUMBER (ParseResult::ERR_BAD_TRIGGER) instead
//            of UB from an unclamped strtod->int32_t cast.
#include "simple_rules.h"
#include "rule_store.h"
#include "event_bus.h"

#include <cstdio>
#include <cstring>
#include <string>

// Stub hooks (host_stubs.cpp).
extern void     stub_rule_store_reset(void);
extern void     stub_rule_store_seed_id(uint16_t id);
extern uint16_t stub_rule_store_count(void);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Build a trigger-only rule whose number literal is `lit`.
static std::string num_rule(const char* lit) {
    return std::string("ON 0x1#temp>") + lit + " DO log hot ENDON";
}

int main() {
    event_bus_init();
    stub_rule_store_reset();

    // ── def 1: id not reused across NVS (uncached persisted rule) ─────────
    // Seed the store with 65 persisted rules ids 1..65 — more than the
    // 64-entry cache can hold — WITHOUT loading them into simple_rules'
    // cache. simple_rules_init() then reloads (load_all stub returns 0, so
    // the cache stays empty), but the store still reports 65 rules.
    for (uint16_t id = 1; id <= 65; id++) stub_rule_store_seed_id(id);
    simple_rules_init();
    CHECK(stub_rule_store_count() == 65, "store seeded with 65 persisted rules");

    // The cache is empty (load_all stub = 0) yet a new rule must get id 66,
    // NOT reuse id 1 (which a cache-only max-scan would have produced).
    uint16_t new_id = 0;
    bool ok = simple_rules_add("fresh", "ON Event#go DO log x ENDON", &new_id);
    CHECK(ok, "add succeeds with empty cache");
    CHECK(new_id == 66,
          "new id derived from store-wide max (66), not reused cache id");

    // Even if we then 'delete' a cached rule, the store-derived allocator
    // must not hand back an id that is still persisted.
    uint16_t new_id2 = 0;
    simple_rules_add("fresh2", "ON Event#go2 DO log y ENDON", &new_id2);
    CHECK(new_id2 == 67 && new_id2 != new_id,
          "second add advances past the previous store-wide max");

    // ── def 4: oversize DSL rejected (no truncate-persist) ────────────────
    // RuleSlot::src is 500 bytes; a 600-byte action body must be rejected,
    // not silently clamped+stored.
    std::string big = "ON Event#x DO log ";
    big += std::string(600, 'A');
    big += " ENDON";
    uint16_t before = stub_rule_store_count();
    uint16_t junk = 0;
    bool too_long = simple_rules_add("big", big.c_str(), &junk);
    CHECK(!too_long, "oversize DSL rejected by simple_rules_add");
    CHECK(stub_rule_store_count() == before,
          "oversize DSL not persisted (count unchanged)");
    CHECK(std::string(dsl_last_error()).find("too long") != std::string::npos,
          "oversize DSL surfaces a 'too long' error message");

    // A DSL whose ACTION section (between " DO " and "ENDON") alone exceeds
    // the 500-byte parse buffer returns ERR_ACTION_TOO_LONG rather than
    // silently clamping to a truncated, differently-parsed action set.
    // dsl_parse is reached directly here (it's the layer that owns the
    // 500-byte actions_str); the > the-buffer length is what matters.
    {
        std::string acts = "ON Event#x DO ";
        acts += std::string(520, 'z');   // 520 > 499-byte actions_str cap
        acts += "ENDON";
        ParsedRule r{};
        ParseResult pr = dsl_parse(acts.c_str(), 1, &r);
        CHECK(pr == ParseResult::ERR_ACTION_TOO_LONG,
              "action section overrun -> ERR_ACTION_TOO_LONG (no clamp)");
    }

    // ── def 5: out-of-range / garbage numeric literals ────────────────────
    {
        ParsedRule r{};
        CHECK(dsl_parse(num_rule("1e20").c_str(), 1, &r) == ParseResult::ERR_BAD_TRIGGER,
              "1e20 numeric literal rejected (out of int32 range)");
        CHECK(dsl_parse(num_rule("-1e20").c_str(), 1, &r) == ParseResult::ERR_BAD_TRIGGER,
              "-1e20 numeric literal rejected");
        CHECK(dsl_parse(num_rule("99999999999").c_str(), 1, &r) == ParseResult::ERR_BAD_TRIGGER,
              "11-digit integer literal rejected (> INT32_MAX)");
        CHECK(dsl_parse(num_rule("notanumber").c_str(), 1, &r) == ParseResult::ERR_BAD_TRIGGER,
              "garbage numeric literal rejected");
        // Sanity: an in-range literal still parses.
        CHECK(dsl_parse(num_rule("25").c_str(), 1, &r) == ParseResult::OK &&
              r.trigger.int_val == 25,
              "in-range literal (25) still parses correctly");
        // Boundary: INT32_MAX must round-trip.
        CHECK(dsl_parse(num_rule("2147483647").c_str(), 1, &r) == ParseResult::OK &&
              r.trigger.int_val == 2147483647,
              "INT32_MAX literal parses to exactly INT32_MAX");
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
