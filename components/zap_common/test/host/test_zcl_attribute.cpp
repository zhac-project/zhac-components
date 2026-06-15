// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host test for zcl_attribute.h (P4-T28, FINDINGS §8): the set helpers
// must never call strncpy(dst, NULL, n) (UB). A null key — or null val —
// must yield a well-defined empty string instead of crashing on the
// decode path. Also exercises normal population + the documented key /
// value truncation behaviour.
//
// ZCL_ATTR_ASSERT_KEY_FITS is intentionally LEFT OFF here (firmware
// default) so over-long keys truncate rather than abort — matches the
// production code path. The null-guard must hold regardless of the
// assert macro.

#include "zcl_attribute.h"

#include <cstring>
#include <cstdio>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

int main() {
    // ── 1. Normal INT population ────────────────────────────────────────
    {
        ZclAttribute a{};
        zcl_attr_set_int(&a, "on_off", 1, VAL_BOOL);
        CHECK(std::strcmp(a.key, "on_off") == 0, "int: key copied");
        CHECK(a.val_type == VAL_BOOL && a.int_val == 1, "int: value/type set");
    }

    // ── 2. Null key on set_int → empty string, no crash ─────────────────
    {
        ZclAttribute a{};
        std::memset(a.key, 0x7F, sizeof(a.key));   // poison
        zcl_attr_set_int(&a, nullptr, 42);
        CHECK(a.key[0] == '\0', "int: null key → empty string (no UB)");
        CHECK(a.int_val == 42, "int: value still set with null key");
    }

    // ── 3. Normal STR population ────────────────────────────────────────
    {
        ZclAttribute a{};
        zcl_attr_set_str(&a, "action", "single");
        CHECK(std::strcmp(a.key, "action") == 0, "str: key copied");
        CHECK(a.val_type == VAL_STR &&
              std::strcmp(a.str_val, "single") == 0, "str: value copied");
    }

    // ── 4. Null key on set_str → empty key, val still copied ────────────
    {
        ZclAttribute a{};
        std::memset(a.key, 0x7F, sizeof(a.key));
        zcl_attr_set_str(&a, nullptr, "kept");
        CHECK(a.key[0] == '\0', "str: null key → empty string (no UB)");
        CHECK(std::strcmp(a.str_val, "kept") == 0, "str: val copied despite null key");
    }

    // ── 5. Null val on set_str → empty string (regression: already guarded)
    {
        ZclAttribute a{};
        zcl_attr_set_str(&a, "k", nullptr);
        CHECK(std::strcmp(a.key, "k") == 0, "str: key copied with null val");
        CHECK(a.str_val[0] == '\0', "str: null val → empty string (no UB)");
    }

    // ── 6. Both null → both empty, no crash ─────────────────────────────
    {
        ZclAttribute a{};
        zcl_attr_set_str(&a, nullptr, nullptr);
        CHECK(a.key[0] == '\0' && a.str_val[0] == '\0',
              "str: null key AND val → both empty (no UB)");
    }

    // ── 7. Over-long key truncates to ATTR_KEY_MAX-1 + NUL ──────────────
    {
        ZclAttribute a{};
        char longkey[64];
        std::memset(longkey, 'x', sizeof(longkey));
        longkey[sizeof(longkey) - 1] = '\0';
        zcl_attr_set_int(&a, longkey, 5);
        CHECK(std::strlen(a.key) == (size_t)(ATTR_KEY_MAX - 1),
              "int: over-long key truncated to ATTR_KEY_MAX-1");
        CHECK(a.key[ATTR_KEY_MAX - 1] == '\0', "int: key NUL-terminated");
    }

    // ── 8. VAL_FLOAT: zcl_attr_set_float stores value×100 tagged VAL_FLOAT ──
    {
        ZclAttribute a{};
        zcl_attr_set_float(&a, "temperature", 26.5f);
        CHECK(a.val_type == VAL_FLOAT, "float: val_type is VAL_FLOAT (not VAL_INT)");
        CHECK(a.int_val == 2650, "float: 26.5 stored as int_val 2650 (x100)");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS",
           s_failures);
    return s_failures ? 1 : 0;
}
