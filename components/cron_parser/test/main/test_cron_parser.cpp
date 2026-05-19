// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "cron_parser.h"
#include <ctime>
#include <cstring>

// ── Helper: build a time_t for a specific date+time (local) ──────────────

static time_t make_time(int year, int mon, int mday, int hour, int min, int wday_unused) {
    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;   // 0-indexed
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

// ── cron_parse: valid expressions ────────────────────────────────────────

TEST_CASE("cron: parse * * * * *", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("* * * * *", e));
    // All minute bits set (0-59)
    for (int i = 0; i < 60; i++)
        TEST_ASSERT_TRUE((e.minute_bits >> i) & 1);
    // All hour bits set (0-23)
    for (int i = 0; i < 24; i++)
        TEST_ASSERT_TRUE((e.hour_bits >> i) & 1);
}

TEST_CASE("cron: parse */5 * * * *", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("*/5 * * * *", e));
    // Bits 0,5,10,...,55 should be set
    for (int i = 0; i < 60; i++) {
        bool expect = (i % 5 == 0);
        TEST_ASSERT_EQUAL(expect, (bool)((e.minute_bits >> i) & 1));
    }
}

TEST_CASE("cron: parse 0 * * * *", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("0 * * * *", e));
    TEST_ASSERT_TRUE ((e.minute_bits >> 0) & 1);
    TEST_ASSERT_FALSE((e.minute_bits >> 1) & 1);
}

TEST_CASE("cron: parse 0 8 * * *", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("0 8 * * *", e));
    TEST_ASSERT_TRUE ((e.hour_bits >> 8) & 1);
    TEST_ASSERT_FALSE((e.hour_bits >> 7) & 1);
    TEST_ASSERT_FALSE((e.hour_bits >> 9) & 1);
}

TEST_CASE("cron: parse range 10-20 in minute field", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("10-20 * * * *", e));
    for (int i = 0; i < 60; i++) {
        bool expect = (i >= 10 && i <= 20);
        TEST_ASSERT_EQUAL(expect, (bool)((e.minute_bits >> i) & 1));
    }
}

TEST_CASE("cron: parse list 0,15,30,45 in minute field", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("0,15,30,45 * * * *", e));
    TEST_ASSERT_TRUE ((e.minute_bits >>  0) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 15) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 30) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 45) & 1);
    TEST_ASSERT_FALSE((e.minute_bits >>  1) & 1);
}

TEST_CASE("cron: parse month field 1-12", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("* * * * *", e));
    for (int m = 1; m <= 12; m++)
        TEST_ASSERT_TRUE((e.month_bits >> m) & 1);
    // bit 0 unused
    TEST_ASSERT_FALSE((e.month_bits >> 0) & 1);
}

TEST_CASE("cron: parse specific month", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("* * * 6 *", e));  // June only
    TEST_ASSERT_TRUE ((e.month_bits >> 6) & 1);
    TEST_ASSERT_FALSE((e.month_bits >> 1) & 1);
    TEST_ASSERT_FALSE((e.month_bits >> 7) & 1);
}

TEST_CASE("cron: parse day-of-week 1 (Monday)", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("* * * * 1", e));  // Monday only
    TEST_ASSERT_TRUE ((e.wday_bits >> 1) & 1);
    TEST_ASSERT_FALSE((e.wday_bits >> 0) & 1);
    TEST_ASSERT_FALSE((e.wday_bits >> 2) & 1);
}

TEST_CASE("cron: range with step 10-50/10", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("10-50/10 * * * *", e));
    TEST_ASSERT_TRUE ((e.minute_bits >> 10) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 20) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 30) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 40) & 1);
    TEST_ASSERT_TRUE ((e.minute_bits >> 50) & 1);
    TEST_ASSERT_FALSE((e.minute_bits >> 15) & 1);
}

// ── cron_parse: error cases ───────────────────────────────────────────────

TEST_CASE("cron: too few fields returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("* * * *", e));
}

TEST_CASE("cron: too many fields returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("* * * * * *", e));
}

TEST_CASE("cron: empty string returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("", e));
}

TEST_CASE("cron: minute out of range returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("60 * * * *", e));
}

TEST_CASE("cron: hour out of range returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("* 24 * * *", e));
}

TEST_CASE("cron: month out of range returns false", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("* * * 13 *", e));
}

// ── cron_matches ──────────────────────────────────────────────────────────

TEST_CASE("cron: * * * * * matches any time", "[cron_parser]") {
    CronExpr e{};
    cron_parse("* * * * *", e);
    time_t t = make_time(2025, 6, 15, 14, 30, 0);
    TEST_ASSERT_TRUE(cron_matches(e, t));
}

TEST_CASE("cron: 0 * * * * matches only minute 0", "[cron_parser]") {
    CronExpr e{};
    cron_parse("0 * * * *", e);
    time_t t_match = make_time(2025, 1, 1, 10,  0, 0);
    time_t t_miss  = make_time(2025, 1, 1, 10, 30, 0);
    TEST_ASSERT_TRUE (cron_matches(e, t_match));
    TEST_ASSERT_FALSE(cron_matches(e, t_miss));
}

TEST_CASE("cron: */5 * * * * matches multiples of 5", "[cron_parser]") {
    CronExpr e{};
    cron_parse("*/5 * * * *", e);
    time_t t_0  = make_time(2025, 1, 1, 10,  0, 0);
    time_t t_5  = make_time(2025, 1, 1, 10,  5, 0);
    time_t t_3  = make_time(2025, 1, 1, 10,  3, 0);
    TEST_ASSERT_TRUE (cron_matches(e, t_0));
    TEST_ASSERT_TRUE (cron_matches(e, t_5));
    TEST_ASSERT_FALSE(cron_matches(e, t_3));
}

TEST_CASE("cron: 0 8 * * * matches 08:00 only", "[cron_parser]") {
    CronExpr e{};
    cron_parse("0 8 * * *", e);
    time_t t_match = make_time(2025, 3, 10,  8,  0, 0);
    time_t t_miss1 = make_time(2025, 3, 10,  9,  0, 0);
    time_t t_miss2 = make_time(2025, 3, 10,  8, 30, 0);
    TEST_ASSERT_TRUE (cron_matches(e, t_match));
    TEST_ASSERT_FALSE(cron_matches(e, t_miss1));
    TEST_ASSERT_FALSE(cron_matches(e, t_miss2));
}

TEST_CASE("cron: day-of-month field matches correctly", "[cron_parser]") {
    CronExpr e{};
    cron_parse("0 0 15 * *", e);  // midnight on the 15th
    time_t t_match = make_time(2025, 4, 15, 0, 0, 0);
    time_t t_miss  = make_time(2025, 4, 14, 0, 0, 0);
    TEST_ASSERT_TRUE (cron_matches(e, t_match));
    TEST_ASSERT_FALSE(cron_matches(e, t_miss));
}

TEST_CASE("cron: month field filters correctly", "[cron_parser]") {
    CronExpr e{};
    cron_parse("0 0 1 12 *", e);  // midnight on Dec 1st
    time_t t_match = make_time(2025, 12, 1, 0, 0, 0);
    time_t t_miss  = make_time(2025, 11, 1, 0, 0, 0);
    TEST_ASSERT_TRUE (cron_matches(e, t_match));
    TEST_ASSERT_FALSE(cron_matches(e, t_miss));
}

// ── 6-field cron with seconds ────────────────────────────────────────

static time_t make_time_s(int year, int mon, int mday, int hour, int min, int sec) {
    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;
    t.tm_isdst = -1;
    return mktime(&t);
}

TEST_CASE("cron: 5-field form fires only at second :00", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("* * * * *", e));
    TEST_ASSERT_EQUAL_UINT64(1ULL << 0, e.second_bits);
    TEST_ASSERT_TRUE (cron_matches(e, make_time_s(2025, 1, 1, 10, 0,  0)));
    TEST_ASSERT_FALSE(cron_matches(e, make_time_s(2025, 1, 1, 10, 0, 17)));
}

TEST_CASE("cron: 6-field */5 * * * * * fires every 5 seconds", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("*/5 * * * * *", e));
    for (int s = 0; s < 60; s++) {
        bool expect = (s % 5 == 0);
        bool got    = (e.second_bits >> s) & 1;
        TEST_ASSERT_EQUAL(expect, got);
    }
    TEST_ASSERT_TRUE (cron_matches(e, make_time_s(2025, 1, 1, 10, 0,  0)));
    TEST_ASSERT_TRUE (cron_matches(e, make_time_s(2025, 1, 1, 10, 0,  5)));
    TEST_ASSERT_TRUE (cron_matches(e, make_time_s(2025, 1, 1, 10, 0, 55)));
    TEST_ASSERT_FALSE(cron_matches(e, make_time_s(2025, 1, 1, 10, 0,  3)));
}

TEST_CASE("cron: 6-field 30 0 8 * * 1-5 fires Mon-Fri at 08:00:30", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_TRUE(cron_parse("30 0 8 * * 1-5", e));
    // 2025-01-06 is Monday.
    TEST_ASSERT_TRUE (cron_matches(e, make_time_s(2025, 1, 6, 8, 0, 30)));
    TEST_ASSERT_FALSE(cron_matches(e, make_time_s(2025, 1, 6, 8, 0,  0)));
    TEST_ASSERT_FALSE(cron_matches(e, make_time_s(2025, 1, 6, 8, 0, 31)));
    // 2025-01-04 is Saturday — should miss.
    TEST_ASSERT_FALSE(cron_matches(e, make_time_s(2025, 1, 4, 8, 0, 30)));
}

TEST_CASE("cron: 7-field input is rejected", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("0 0 0 1 1 1 2025", e));
}

TEST_CASE("cron: 4-field input is rejected", "[cron_parser]") {
    CronExpr e{};
    TEST_ASSERT_FALSE(cron_parse("* * * *", e));
}

// ── F-06: DOM/DOW POSIX 4-quadrant semantics ─────────────────────────────
// Reference table (cron(8) / Vixie cron):
//   both `*`     → match (no day restriction)
//   only DOM `*` → DOW gates alone
//   only DOW `*` → DOM gates alone
//   both set     → OR (fire if EITHER matches)
//
// 2025-05-01 is a Thursday (wday=4). 2025-05-05 is a Monday (wday=1).
// 2025-05-12 is a Monday but day=12. 2025-05-02 is Friday day=2.

TEST_CASE("cron F-06: * * * * * fires any day", "[cron_parser]") {
    CronExpr e{};
    cron_parse("* * * * *", e);
    TEST_ASSERT_TRUE(cron_matches(e, make_time(2025, 5,  1, 12, 0, 0)));
    TEST_ASSERT_TRUE(cron_matches(e, make_time(2025, 5,  5, 12, 0, 0)));
}

TEST_CASE("cron F-06: * * 1 * * fires only on day 1 of month (DOW *)", "[cron_parser]") {
    CronExpr e{};
    cron_parse("* * 1 * *", e);
    // 2025-05-01 is the 1st — match.
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5,  1, 12, 0, 0)));
    // 2025-05-05 is Monday but day 5 — must miss.
    TEST_ASSERT_FALSE(cron_matches(e, make_time(2025, 5,  5, 12, 0, 0)));
    // 2025-05-12 is Monday but day 12 — must miss.
    TEST_ASSERT_FALSE(cron_matches(e, make_time(2025, 5, 12, 12, 0, 0)));
}

TEST_CASE("cron F-06: * * * * 1 fires only on Mondays (DOM *)", "[cron_parser]") {
    CronExpr e{};
    cron_parse("* * * * 1", e);
    // 2025-05-05 is Monday — match.
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5,  5, 12, 0, 0)));
    // 2025-05-12 is Monday — match.
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5, 12, 12, 0, 0)));
    // 2025-05-01 is Thursday — must miss.
    TEST_ASSERT_FALSE(cron_matches(e, make_time(2025, 5,  1, 12, 0, 0)));
}

TEST_CASE("cron F-06: * * 1 * 1 POSIX OR — fires day 1 OR Monday", "[cron_parser]") {
    CronExpr e{};
    cron_parse("* * 1 * 1", e);
    // 2025-05-01 is Thursday day 1 — match (DOM hits).
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5,  1, 12, 0, 0)));
    // 2025-05-05 is Monday day 5 — match (DOW hits).
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5,  5, 12, 0, 0)));
    // 2025-05-12 is Monday day 12 — match (DOW hits).
    TEST_ASSERT_TRUE (cron_matches(e, make_time(2025, 5, 12, 12, 0, 0)));
    // 2025-05-02 is Friday day 2 — must miss (neither matches).
    TEST_ASSERT_FALSE(cron_matches(e, make_time(2025, 5,  2, 12, 0, 0)));
}

TEST_CASE("cron F-06: cron_next for * * 1 * 1 advances to next matching day", "[cron_parser]") {
    CronExpr e{};
    cron_parse("0 0 1 * 1", e);  // 00:00 on day 1 OR Mondays
    // Start Wed 2025-04-30 12:00. Next match: Thu 2025-05-01 00:00 (day 1).
    time_t start = make_time(2025, 4, 30, 12, 0, 0);
    time_t next = cron_next(e, start);
    TEST_ASSERT_NOT_EQUAL(0, next);
    struct tm tm_n{}; localtime_r(&next, &tm_n);
    TEST_ASSERT_EQUAL(2025 - 1900, tm_n.tm_year);
    TEST_ASSERT_EQUAL(5 - 1,       tm_n.tm_mon);
    TEST_ASSERT_EQUAL(1,           tm_n.tm_mday);
    TEST_ASSERT_EQUAL(0,           tm_n.tm_hour);
}
