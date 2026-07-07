// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Comprehensive cron_parser coverage — grammar, field ranges, matching, the
// POSIX DOM/DOW OR semantics, and cron_next. Complements test_cron, which owns
// the P2-T18 over-length / leap-second regressions.
//
// TZ is pinned to UTC so mktime()/localtime_r() round-trip without DST
// ambiguity. Weekday-dependent cases derive the target (mday, wday) from the
// constructed time rather than hardcoding a calendar weekday.
#include "cron_parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static bool ok(const char* expr) { CronExpr e{}; return cron_parse(expr, e); }

static time_t mk(int y, int mon, int day, int h, int mi, int s) {
    struct tm t{};
    t.tm_year = y - 1900; t.tm_mon = mon - 1; t.tm_mday = day;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = -1;
    return mktime(&t);
}
static bool matches(const char* expr, time_t t) {
    CronExpr e{};
    if (!cron_parse(expr, e)) { printf("  (parse failed: %s)\n", expr); return false; }
    return cron_matches(e, t);
}

int main() {
    setenv("TZ", "UTC", 1); tzset();

    // ── A. Parse: field count + baselines ────────────────────────────────
    CHECK( ok("0 8 * * *"),        "5-field parses");
    CHECK( ok("30 0 8 * * *"),     "6-field parses");
    CHECK(!ok("0 8 * *"),          "4 fields rejected");
    CHECK(!ok("0 0 8 * * * *"),    "7 fields rejected");
    CHECK(!ok(""),                 "empty string rejected");
    CHECK( ok("0   8 * * *"),      "multiple spaces between fields tolerated");

    // ── B. Parse: per-field ranges (boundary + just-over) ────────────────
    CHECK( ok("59 8 * * *")  && !ok("60 8 * * *"),  "minute range 0-59");
    CHECK( ok("0 23 * * *")  && !ok("0 24 * * *"),  "hour range 0-23");
    CHECK( ok("0 8 31 * *")  && !ok("0 8 32 * *") && !ok("0 8 0 * *"), "mday range 1-31");
    CHECK( ok("0 8 * 12 *")  && !ok("0 8 * 13 *") && !ok("0 8 * 0 *"), "month range 1-12");
    CHECK( ok("0 8 * * 0")   && ok("0 8 * * 6")  && !ok("0 8 * * 7"),  "wday range 0-6 (Sun=0)");
    CHECK( ok("30 0 8 * * *") && !ok("60 0 8 * * *"), "second range 0-59 (6-field)");

    // ── C. Parse: syntax (*, */N, ranges, lists, rejects) ────────────────
    CHECK( ok("0 */15 * * *"),        "*/step accepted");
    CHECK(!ok("0 */0 * * *"),         "*/0 (zero step) rejected");
    CHECK( ok("0 8-10 * * *"),        "N-M range accepted");
    CHECK(!ok("0 10-5 * * *"),        "reversed range (lo>hi) rejected");
    CHECK( ok("0,15,30,45 8 * * *"),  "comma list accepted");
    CHECK( ok("0 8-16/2 * * *"),      "range with step accepted");
    CHECK(!ok("x 8 * * *"),           "non-numeric field rejected");
    CHECK(!ok("0 8 * * mon"),         "named weekday rejected (numeric only)");

    // ── D. Match: per-field ──────────────────────────────────────────────
    const time_t t800   = mk(2026, 6, 10, 8, 0, 0);   // Jun 10 2026, 08:00:00
    CHECK( matches("0 8 * * *", t800),                 "min0 hour8 matches 08:00:00");
    CHECK(!matches("0 8 * * *", mk(2026,6,10,8,0,30)), "5-field fires only at second 0");
    CHECK(!matches("0 8 * * *", mk(2026,6,10,8,1,0)),  "minute mismatch → no match");
    CHECK(!matches("0 8 * * *", mk(2026,6,10,9,0,0)),  "hour mismatch → no match");
    CHECK( matches("30 0 8 * * *", mk(2026,6,10,8,0,30)) &&
          !matches("30 0 8 * * *", t800),              "6-field second field matches at :30, not :00");
    CHECK( matches("0,30 8 * * *", mk(2026,6,10,8,30,0)) &&
          !matches("0,30 8 * * *", mk(2026,6,10,8,15,0)), "minute list {0,30}");
    CHECK( matches("0 8-10 * * *", mk(2026,6,10,9,0,0)) &&
          !matches("0 8-10 * * *", mk(2026,6,10,11,0,0)), "hour range 8-10");
    CHECK( matches("*/15 8 * * *", mk(2026,6,10,8,15,0)) &&
          !matches("*/15 8 * * *", mk(2026,6,10,8,10,0)), "minute step */15");
    CHECK( matches("0 8 * 6 *", t800) && !matches("0 8 * 6 *", mk(2026,7,10,8,0,0)),
          "month field selects June only");

    // ── E. POSIX DOM/DOW OR semantics (LUA-F4) ───────────────────────────
    // Derive (mday, wday) from t800 so no weekday is hardcoded.
    struct tm bt{}; localtime_r(&t800, &bt);
    const int md = bt.tm_mday, wd = bt.tm_wday;
    const int md1 = md + 1, wd1 = (wd + 1) % 7;   // a different, valid mday/wday
    char b[64];
    snprintf(b, sizeof b, "0 8 %d * %d", md, wd1);
    CHECK( matches(b, t800), "DOM & DOW both set: fires via DOM alone (OR, not AND)");
    snprintf(b, sizeof b, "0 8 %d * %d", md1, wd);
    CHECK( matches(b, t800), "DOM & DOW both set: fires via DOW alone (OR)");
    snprintf(b, sizeof b, "0 8 %d * %d", md1, wd1);
    CHECK(!matches(b, t800), "DOM & DOW both set: neither matches → no fire");
    snprintf(b, sizeof b, "0 8 %d * *", md);
    CHECK( matches(b, t800), "DOW=* : gated by DOM (matches)");
    snprintf(b, sizeof b, "0 8 %d * *", md1);
    CHECK(!matches(b, t800), "DOW=* : DOM mismatch → no fire");
    snprintf(b, sizeof b, "0 8 * * %d", wd);
    CHECK( matches(b, t800), "DOM=* : gated by DOW (matches)");
    snprintf(b, sizeof b, "0 8 * * %d", wd1);
    CHECK(!matches(b, t800), "DOM=* : DOW mismatch → no fire");
    CHECK( matches("0 8 * * *", t800), "DOM=* DOW=* : any day matches");

    // ── F. cron_next ─────────────────────────────────────────────────────
    {
        CronExpr e{};
        cron_parse("0 30 8 * * *", e);            // 08:30:00 daily
        time_t nxt = cron_next(e, t800);          // from 08:00:00
        struct tm o{}; localtime_r(&nxt, &o);
        CHECK(nxt > t800 && o.tm_hour == 8 && o.tm_min == 30 && o.tm_sec == 0 && o.tm_mday == md,
              "cron_next finds the same-day 08:30:00 slot");

        time_t after = cron_next(e, mk(2026, 6, 10, 8, 31, 0));  // past today's slot
        struct tm o2{}; localtime_r(&after, &o2);
        CHECK(o2.tm_mday == md + 1 && o2.tm_hour == 8 && o2.tm_min == 30,
              "cron_next wraps to next day when today's slot has passed");

        CronExpr feb30{};
        CHECK(cron_parse("0 0 0 30 2 *", feb30), "Feb-30 expression parses");
        CHECK(cron_next(feb30, t800) == 0, "impossible date (Feb 30) → cron_next returns 0");
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
