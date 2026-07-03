// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression for Bool-attribute trigger matching.
//
// A binary expose (occupancy, contact, water_leak, tamper, …) reaches the
// shadow tagged VAL_BOOL with int_val = 0/1 (the shadow bridge normalises the
// bool into int_val). The DSL writes such triggers as `#occupancy=1` / `=0`,
// which dsl_parse stores as a VAL_INT literal. The matcher's type gate only
// folded VAL_FLOAT→VAL_INT, so a VAL_INT literal vs a VAL_BOOL event failed the
// `match_val_type != attr_vt` gate before compare_int ran — meaning NO bool
// trigger could ever fire (every motion/contact rule was silently dead).
//
// This pins the fix: VAL_BOOL folds into the integer comparison domain exactly
// like VAL_FLOAT, so `=1`/`=0` match a bool, while the Int/Float/Str paths are
// unchanged and an int literal still must not match a string attribute.
#include "simple_rules.h"
#include "event_bus.h"

#include <cstdio>
#include <cstring>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Build a ZCL_ATTR event carrying one decoded attribute. trigger.ieee stays 0
// (unresolved friendly name = wildcard), so the matcher skips the ieee check.
static Event make_attr_event(const char* key, uint8_t val_type,
                             int32_t int_val, const char* str_val) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = 0xa4c138d5f501d501ULL;   // arbitrary source device
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

// Parse `dsl` into a rule, then run the matcher against `ev`.
static bool parse_and_match(const char* dsl, const Event& ev) {
    ParsedRule rule{};
    if (dsl_parse(dsl, 1, &rule) != ParseResult::OK) {
        printf("  (parse failed for '%s': %s)\n", dsl, dsl_last_error());
        return false;
    }
    char buf[64];
    return simple_rules_match(rule, ev, buf, sizeof buf);
}

int main() {
    // ── The bug: a Bool occupancy report vs `=1` / `=0` ──────────────────
    const Event occ_true  = make_attr_event("occupancy", VAL_BOOL, 1, nullptr);
    const Event occ_false = make_attr_event("occupancy", VAL_BOOL, 0, nullptr);

    CHECK(parse_and_match("ON dev#occupancy=1 DO log m ENDON", occ_true),
          "occupancy=1 matches a VAL_BOOL true report");
    CHECK(parse_and_match("ON dev#occupancy=0 DO log m ENDON", occ_false),
          "occupancy=0 matches a VAL_BOOL false report");

    // ── Correct negatives: the fold must not match the wrong value ───────
    CHECK(!parse_and_match("ON dev#occupancy=1 DO log m ENDON", occ_false),
          "occupancy=1 does NOT match a false report");
    CHECK(!parse_and_match("ON dev#occupancy=0 DO log m ENDON", occ_true),
          "occupancy=0 does NOT match a true report");

    // ── Regressions: Int / Float / Str comparison paths unchanged ────────
    const Event lvl = make_attr_event("level", VAL_INT, 50, nullptr);
    CHECK(parse_and_match("ON dev#level=50 DO log m ENDON", lvl),
          "VAL_INT equality still matches (regression)");

    // VAL_FLOAT stores value × 100; `>2500` is 25.00 — 26.00 must exceed it.
    const Event temp = make_attr_event("temperature", VAL_FLOAT, 2600, nullptr);
    CHECK(parse_and_match("ON dev#temperature>2500 DO log m ENDON", temp),
          "VAL_FLOAT x100 compare still matches (regression)");

    const Event act = make_attr_event("action", VAL_STR, 0, "single");
    CHECK(parse_and_match("ON dev#action=\"single\" DO log m ENDON", act),
          "VAL_STR equality still matches (regression)");

    // ── Guard: an int literal must NOT leak into matching a string attr ──
    const Event occ_str = make_attr_event("occupancy", VAL_STR, 0, "presence");
    CHECK(!parse_and_match("ON dev#occupancy=1 DO log m ENDON", occ_str),
          "int literal =1 does NOT match a VAL_STR attr");

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
