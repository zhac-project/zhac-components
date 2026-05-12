// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "cron_parser.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

// ── Field parser ──────────────────────────────────────────────────────────
// Parses one cron field into a uint64_t bitmask.
// min_val/max_val: inclusive value range.
// Values map directly to bit positions (bit N = value N).

// Parse a decimal integer from s, advancing *end past the digits.
// Returns false if s contains no valid digits or has trailing non-digit chars.
static bool parse_int(const char* s, int* out) {
    if (!s || *s == '\0') return false;
    char* end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (int)v;
    return true;
}

static bool parse_field(const char* s, int min_val, int max_val,
                         uint64_t* bits, bool* is_star = nullptr) {
    *bits = 0;
    if (is_star) *is_star = false;

    // "*" or "*/step" — both count as "wildcard" for POSIX OR semantics
    // (`*/N` still matches every Nth slot, but the field is unrestricted
    // so the LUA-F4 OR rule applies to DOM and DOW).
    if (s[0] == '*') {
        int step = 1;
        if (s[1] == '/') {
            if (!parse_int(s + 2, &step) || step <= 0) return false;
        } else if (s[1] != '\0') {
            return false; // unexpected char after *
        }
        for (int v = min_val; v <= max_val; v += step)
            *bits |= (1ULL << v);
        if (is_star) *is_star = true;
        return true;
    }

    // Comma-separated list of: N  |  N-M  |  N-M/step
    char buf[128];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* save = nullptr;
    char* tok = strtok_r(buf, ",", &save);
    while (tok) {
        char* dash = strchr(tok, '-');
        if (dash) {
            *dash = '\0';
            int lo = 0;
            if (!parse_int(tok, &lo)) return false;
            int step = 1;
            char* slash = strchr(dash + 1, '/');
            int hi = 0;
            if (slash) {
                *slash = '\0';
                if (!parse_int(dash + 1, &hi)) return false;
                if (!parse_int(slash + 1, &step) || step <= 0) return false;
            } else {
                if (!parse_int(dash + 1, &hi)) return false;
            }
            if (lo < min_val || hi > max_val || lo > hi) return false;
            for (int v = lo; v <= hi; v += step)
                *bits |= (1ULL << v);
        } else {
            int v = 0;
            if (!parse_int(tok, &v)) return false;
            if (v < min_val || v > max_val) return false;
            *bits |= (1ULL << v);
        }
        tok = strtok_r(nullptr, ",", &save);
    }
    return (*bits != 0);
}

// ── Public API ────────────────────────────────────────────────────────────

bool cron_parse(const char* expr, CronExpr& out) {
    if (!expr || !*expr) return false;

    char buf[128];
    strncpy(buf, expr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Split into exactly 5 fields by whitespace
    const char* fields[5] = {};
    char* p = buf;
    int n = 0;
    while (*p && n < 5) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        fields[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    if (n != 5) return false;

    uint64_t bits = 0;

    // minute: 0–59
    if (!parse_field(fields[0], 0, 59, &bits)) return false;
    out.minute_bits = bits;

    // hour: 0–23
    if (!parse_field(fields[1], 0, 23, &bits)) return false;
    out.hour_bits = (uint32_t)bits;

    // day-of-month: 1–31 (bit 1 = 1st). Track `*` for OR semantics.
    bool dom_star = false;
    if (!parse_field(fields[2], 1, 31, &bits, &dom_star)) return false;
    out.day_bits = (uint32_t)bits;

    // month: 1–12 (bit 1 = January)
    if (!parse_field(fields[3], 1, 12, &bits)) return false;
    out.month_bits = (uint16_t)bits;

    // day-of-week: 0–6 (0 = Sunday). Track `*` for OR semantics.
    bool dow_star = false;
    if (!parse_field(fields[4], 0, 6, &bits, &dow_star)) return false;
    out.wday_bits = (uint8_t)bits;

    out.flags = (uint8_t)((dom_star ? CRON_FLAG_DOM_STAR : 0) |
                          (dow_star ? CRON_FLAG_DOW_STAR : 0));
    return true;
}

bool cron_matches(const CronExpr& expr, time_t t) {
    struct tm tm_val{};
    localtime_r(&t, &tm_val);

    int minute = tm_val.tm_min;       // 0–59
    int hour   = tm_val.tm_hour;      // 0–23
    int day    = tm_val.tm_mday;      // 1–31
    int month  = tm_val.tm_mon + 1;   // 1–12
    int wday   = tm_val.tm_wday;      // 0–6 (Sunday = 0)

    if (!(expr.minute_bits & (1ULL << minute))) return false;
    if (!(expr.hour_bits   & (1UL  << hour)))   return false;
    if (!(expr.month_bits  & (1U   << month)))   return false;

    // POSIX/Vixie cron DOM/DOW semantics (LUA-F4): if either field is
    // `*` AND today's value matches; otherwise OR them so a restricted
    // pair like "1 * 1" fires on the 1st of the month *and* every
    // Monday — not only on Mondays that fall on the 1st.
    bool dom_match = (expr.day_bits  & (1UL << day))  != 0;
    bool dow_match = (expr.wday_bits & (1U  << wday)) != 0;
    bool dom_star  = (expr.flags & CRON_FLAG_DOM_STAR) != 0;
    bool dow_star  = (expr.flags & CRON_FLAG_DOW_STAR) != 0;

    if (dom_star || dow_star) {
        if (!dom_match || !dow_match) return false;     // AND when either is wildcard
    } else {
        if (!dom_match && !dow_match) return false;     // OR when both restricted
    }
    return true;
}

time_t cron_next(const CronExpr& expr, time_t from_t) {
    // Start at the next full minute strictly after from_t
    time_t t = ((from_t / 60) + 1) * 60;
    const time_t limit = from_t + (time_t)4 * 365 * 24 * 3600;

    struct tm tm_val{};
    while (t < limit) {
        localtime_r(&t, &tm_val);

        // Check month; if wrong, jump to 1st of next month at 00:00
        int month = tm_val.tm_mon + 1;
        if (!(expr.month_bits & (1U << month))) {
            tm_val.tm_mon++;
            tm_val.tm_mday  = 1;
            tm_val.tm_hour  = 0;
            tm_val.tm_min   = 0;
            tm_val.tm_sec   = 0;
            tm_val.tm_isdst = -1;
            t = mktime(&tm_val);
            if (t == (time_t)-1) return 0;
            continue;
        }

        // Check DOM/DOW with POSIX OR semantics (LUA-F4): only restrict
        // when both fields are non-`*`; otherwise the wildcard side is
        // satisfied automatically and we AND with the restricted side.
        int day  = tm_val.tm_mday;
        int wday = tm_val.tm_wday;
        bool dom_match = (expr.day_bits  & (1UL << day))  != 0;
        bool dow_match = (expr.wday_bits & (1U  << wday)) != 0;
        bool dom_star  = (expr.flags & CRON_FLAG_DOM_STAR) != 0;
        bool dow_star  = (expr.flags & CRON_FLAG_DOW_STAR) != 0;
        bool day_ok = (dom_star || dow_star) ? (dom_match && dow_match)
                                              : (dom_match || dow_match);
        if (!day_ok) {
            tm_val.tm_mday++;
            tm_val.tm_hour  = 0;
            tm_val.tm_min   = 0;
            tm_val.tm_sec   = 0;
            tm_val.tm_isdst = -1;
            t = mktime(&tm_val);
            if (t == (time_t)-1) return 0;
            continue;
        }

        // Check hour; if wrong, jump to top of next hour
        int hour = tm_val.tm_hour;
        if (!(expr.hour_bits & (1UL << hour))) {
            tm_val.tm_hour++;
            tm_val.tm_min   = 0;
            tm_val.tm_sec   = 0;
            tm_val.tm_isdst = -1;
            t = mktime(&tm_val);
            if (t == (time_t)-1) return 0;
            continue;
        }

        // Check minute; if wrong, advance by one minute
        int minute = tm_val.tm_min;
        if (!(expr.minute_bits & (1ULL << minute))) {
            t += 60;
            continue;
        }

        return t;
    }

    return 0;  // no match within 4 years
}
