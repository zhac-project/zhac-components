// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// P2-T18 host regressions for cron_parser (def 6, FINDINGS §7):
//   - an over-length expression (>= 128 chars) is REJECTED, not truncated
//     into a wrong-but-parseable schedule;
//   - an over-length single field is likewise rejected;
//   - cron_next() terminates even when the clock reports a leap second
//     (tm_sec == 60), which previously made (60 - tm_sec) == 0 and spun
//     the loop forever.
#include "cron_parser.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

int main() {
    // ── valid baselines ───────────────────────────────────────────────────
    CronExpr e{};
    CHECK(cron_parse("0 8 * * *", e), "valid 5-field expression parses");
    CHECK(cron_parse("30 0 8 * * *", e), "valid 6-field expression parses");

    // ── def 6: over-length expression rejected ────────────────────────────
    // 128+ chars: a long comma list that, if truncated to 127, would still
    // split into valid fields and parse into a DIFFERENT minute set.
    {
        std::string minutes = "0";
        for (int m = 1; m <= 59; m++) { minutes += ","; minutes += std::to_string(m); }
        std::string expr = minutes + " 8 * * *";   // well over 128 chars
        CHECK(expr.size() >= 128, "constructed expr is >=128 chars");
        CronExpr ov{};
        CHECK(!cron_parse(expr.c_str(), ov),
              "over-length cron expression rejected (not truncated)");
    }

    // ── def 6: over-length single field rejected ──────────────────────────
    {
        std::string field = "0";
        for (int i = 0; i < 200; i++) field += ",0";   // 400+ chars, one field
        std::string expr = field + " 8 * * *";
        CronExpr ov{};
        CHECK(!cron_parse(expr.c_str(), ov),
              "over-length single field rejected");
    }

    // ── def 6: cron_next terminates on a leap-second clock ─────────────────
    // Construct a time whose tm_sec == 60 and feed it to cron_next with an
    // expression that does NOT match the current minute, forcing the
    // (60 - tm_sec) minute-advance branch. Before the clamp this looped
    // forever; now it must return a finite, strictly-later time.
    {
        CronExpr every_min{};
        // Fire at second 0 of minute 30 every hour — guarantees the minute
        // branch runs for most start minutes.
        CHECK(cron_parse("0 30 * * * *", every_min), "leap-test expr parses");

        struct tm tmv{};
        tmv.tm_year = 2026 - 1900;
        tmv.tm_mon  = 5;    // June
        tmv.tm_mday = 30;
        tmv.tm_hour = 10;
        tmv.tm_min  = 15;   // != 30, so the minute-advance branch fires
        tmv.tm_sec  = 60;   // leap second
        tmv.tm_isdst = -1;
        time_t base = mktime(&tmv);

        time_t next = cron_next(every_min, base);
        CHECK(next != 0,   "cron_next returns a match (no infinite loop) at tm_sec==60");
        CHECK(next > base, "cron_next advances strictly past the start time");
        if (next != 0) {
            struct tm out{};
            localtime_r(&next, &out);
            CHECK(out.tm_min == 30 && out.tm_sec == 0,
                  "cron_next lands on minute 30 second 0");
        }
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
