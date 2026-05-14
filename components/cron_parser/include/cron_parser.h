// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <ctime>

// Cron expression evaluator. Accepts both classic 5-field
// (`min hour mday month wday`) and the 6-field variant with seconds
// (`sec min hour mday month wday`). The 5-field form is treated as
// "second 0 only" — i.e. the legacy minute-resolution behaviour.

// `flags` records whether DOM and DOW were specified as `*` so
// cron_matches can apply POSIX/Vixie semantics: when either field is
// `*`, AND today; otherwise OR the two so `0 8 1 * 1` fires every 1st
// of the month *and* every Monday (LUA-F4 in docs/FINDINGS.md).
static constexpr uint8_t CRON_FLAG_DOM_STAR = 0x01;
static constexpr uint8_t CRON_FLAG_DOW_STAR = 0x02;

struct CronExpr {
    uint64_t second_bits;  // bits 0–59 — defaults to bit 0 only (5-field form)
    uint64_t minute_bits;  // bits 0–59 set for each matched minute
    uint32_t hour_bits;
    uint32_t day_bits;
    uint16_t month_bits;
    uint8_t  wday_bits;
    uint8_t  flags;        // CRON_FLAG_*
};

// Parse a 5- or 6-field cron string into a CronExpr. Returns false on
// parse error.
bool cron_parse(const char* expr, CronExpr& out);

// Returns true if the given time_t matches the CronExpr.
bool cron_matches(const CronExpr& expr, time_t t);

// Returns the next time_t strictly after from_t that matches expr,
// or 0 if no match is found within 4 years.
time_t cron_next(const CronExpr& expr, time_t from_t);
