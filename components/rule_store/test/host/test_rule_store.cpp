// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host coverage for rule_store — the NVS-backed rule persistence layer and its
// write-coalescing overlay. Runs against an in-memory NVS stub (stubs/), so it
// exercises the real rule_store.cpp + rule_store_flush.cpp:
//   • save / load round-trip incl. every RuleSlot field + CRC;
//   • update-in-place (same id overwrites, no duplicate), delete, count, max_id;
//   • load_all iteration incl. skipping a CRC-corrupt blob;
//   • schema-version mismatch wipes all rules on init;
//   • overlay/flush: mark_dirty is visible to load() before the flush and
//     lands in NVS after flush_now(); mark_delete tombstones (load returns
//     not-found) before the flush removes it from NVS.
#include "rule_store.h"

#include <cstdio>
#include <cstring>

extern "C" void nvs_stub_reset(void);
extern "C" int  nvs_stub_rule_count(void);
extern "C" void nvs_stub_set_schema(uint16_t v);
extern "C" bool nvs_stub_corrupt_first_rule(void);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static RuleSlot make_slot(uint16_t id, const char* name, const char* src, bool enabled) {
    RuleSlot s{};
    s.rule_id      = id;
    s.enabled      = enabled ? 1 : 0;
    s.trigger_type = 0;
    s.rule_type    = 0;
    strncpy(s.name, name, sizeof(s.name) - 1);
    size_t n = strlen(src);
    if (n > sizeof(s.src)) n = sizeof(s.src);
    memcpy(s.src, src, n);
    s.src_len = (uint16_t)n;
    // crc32 left 0 — rule_store_save computes it.
    return s;
}

int main() {
    RuleSlot out{};

    // ── Store layer: save / load round-trip ──────────────────────────────
    nvs_stub_reset();
    rule_store_init();

    RuleSlot a = make_slot(1, "rule-a", "ON x#s=1 DO log m ENDON", true);
    CHECK(rule_store_save(&a), "save rule 1 succeeds");
    CHECK(rule_store_load(1, &out) && out.rule_id == 1 && out.enabled == 1 &&
          strcmp(out.name, "rule-a") == 0 && out.src_len == a.src_len &&
          memcmp(out.src, a.src, a.src_len) == 0,
          "load 1 round-trips all RuleSlot fields");
    CHECK(!rule_store_load(999, &out), "load of an unknown id returns false");

    // ── count / max_id / update-in-place ─────────────────────────────────
    RuleSlot b = make_slot(5, "rule-b", "ON y#s=1 DO log m ENDON", false);
    rule_store_save(&b);
    CHECK(rule_store_count() == 2,  "count == 2 after two distinct saves");
    CHECK(rule_store_max_id() == 5, "max_id == 5");

    RuleSlot a2 = make_slot(1, "rule-a2", "ON z#s=1 DO log m ENDON", false);
    rule_store_save(&a2);
    CHECK(rule_store_load(1, &out) && strcmp(out.name, "rule-a2") == 0 && out.enabled == 0,
          "saving the same id overwrites in place");
    CHECK(rule_store_count() == 2, "update does not create a duplicate (count still 2)");

    // ── delete ───────────────────────────────────────────────────────────
    CHECK(rule_store_delete(1),  "delete rule 1 succeeds");
    CHECK(!rule_store_load(1, &out), "deleted rule is not loadable");
    CHECK(!rule_store_delete(1), "delete of an already-gone id returns false");
    CHECK(rule_store_count() == 1, "count == 1 after delete");

    // ── load_all + CRC-corrupt skip ──────────────────────────────────────
    RuleSlot c = make_slot(7, "rule-c", "ON w#s=1 DO log m ENDON", true);
    rule_store_save(&c);
    {
        RuleSlot all[8]{};
        uint16_t n = rule_store_load_all(all, 8);
        CHECK(n == 2, "load_all returns both surviving rules");
    }
    CHECK(nvs_stub_corrupt_first_rule(), "corrupt one stored rule blob");
    {
        RuleSlot all[8]{};
        uint16_t n = rule_store_load_all(all, 8);
        CHECK(n == 1, "load_all skips the CRC-corrupt blob");
    }

    // ── schema-version mismatch wipes on init ────────────────────────────
    nvs_stub_reset();
    rule_store_init();
    RuleSlot e = make_slot(3, "rule-e", "ON x#s=1 DO log m ENDON", true);
    rule_store_save(&e);
    CHECK(rule_store_count() == 1, "one rule before schema bump");
    nvs_stub_set_schema(99);              // simulate a different on-disk schema
    rule_store_init();                    // re-init detects the mismatch
    CHECK(rule_store_count() == 0, "schema-version mismatch wipes all rules");

    // ── overlay / flush ──────────────────────────────────────────────────
    nvs_stub_reset();
    rule_store_init();
    rule_store_flush_init();              // enables the overlay (flush task is a no-op)

    RuleSlot d = make_slot(9, "rule-d", "ON q#s=1 DO log m ENDON", true);
    rule_store_mark_dirty(&d);
    CHECK(rule_store_load(9, &out) && strcmp(out.name, "rule-d") == 0,
          "load reads the dirty overlay before flush");
    CHECK(nvs_stub_rule_count() == 0, "dirty write is not yet in NVS");
    rule_store_flush_now();
    CHECK(nvs_stub_rule_count() == 1 && rule_store_load(9, &out) && strcmp(out.name, "rule-d") == 0,
          "flush_now persists the dirty write to NVS");

    rule_store_mark_delete(9);
    CHECK(!rule_store_load(9, &out), "mark_delete tombstones (load returns not-found) pre-flush");
    CHECK(nvs_stub_rule_count() == 1, "tombstone not yet applied to NVS");
    rule_store_flush_now();
    CHECK(!rule_store_load(9, &out) && nvs_stub_rule_count() == 0,
          "flush_now applies the tombstone (gone from NVS)");

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
