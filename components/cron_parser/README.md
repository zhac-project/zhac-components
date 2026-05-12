# cron_parser — 5-field Cron Evaluator (P4)

Tiny POSIX/Vixie-style cron expression evaluator. Parses
`minute hour day month weekday` into bitmasks, then matches against a
`time_t` or finds the next matching minute. Used by `simple_rules`
TIME_CRON triggers and by Lua's `zhac.on_cron`.

## Where it sits

```
DSL "Rules#Cron=*/5 * * * *"          Lua  zhac.on_cron("0 8 * * 1-5", …)
            │                                          │
            ▼                                          ▼
                cron_parse() → CronExpr (18 B)
                            │
                            ├─► cron_matches(expr, now) ─► simple_rules task_cron
                            └─► cron_next(expr, from)    ─► next-fire calculation
```

P4 only. Pure C++17, no FreeRTOS / NVS dependencies — host-testable.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` only; `cron_parser.h` pulls `<cstdint>` + `<ctime>`.

## Public API (`include/cron_parser.h`)

| Symbol | Notes |
|---|---|
| `bool cron_parse(const char* expr, CronExpr& out)` | Parse a 5-field expression. Returns false on any field error. Sets `flags` to remember whether DOM and DOW were `*`. |
| `bool cron_matches(const CronExpr&, time_t t)` | True iff `t` matches the expression. POSIX OR semantics for DOM/DOW (see below). |
| `time_t cron_next(const CronExpr&, time_t from_t)` | Next matching `time_t` strictly after `from_t`, or `0` if no match within 4 years. Stride is one minute; uses `localtime_r`. |

## Important constants & sizes

| Symbol | Value | Source |
|---|---|---|
| `CRON_FLAG_DOM_STAR` | `0x01` — bit set when DOM field was literal `*` | `cron_parser.h` |
| `CRON_FLAG_DOW_STAR` | `0x02` — bit set when DOW field was literal `*` | `cron_parser.h` |
| `sizeof(CronExpr)` | **18 B** (was 17 B pre-flags) | header |
| `cron_next` search horizon | 4 years (`from_t + 4 * 365 * 86400`) | `cron_parser.cpp` |

```c
struct CronExpr {
    uint64_t minute_bits;  // bits 0..59
    uint32_t hour_bits;    // bits 0..23
    uint32_t day_bits;     // bits 1..31
    uint16_t month_bits;   // bits 1..12 (January = bit 1)
    uint8_t  wday_bits;    // bits 0..6 (Sunday = bit 0)
    uint8_t  flags;        // CRON_FLAG_DOM_STAR | CRON_FLAG_DOW_STAR
};
```

## Supported syntax

Standard 5-field cron, one expression per field:

| Field | Range | Notes |
|---|---|---|
| minute | 0–59 | |
| hour | 0–23 | |
| day-of-month | 1–31 | |
| month | 1–12 | |
| day-of-week | 0–6 | 0 = Sunday |

Each field accepts: `*`, single value (`5`), list (`1,3,5`), range
(`9-17`), step (`*/15`, `0-30/5`).

Examples:

| Expression | Meaning |
|---|---|
| `*/5 * * * *` | every 5 min |
| `0 8 * * 1-5` | weekdays at 08:00 |
| `0 8 1 * *` | the 1st of each month at 08:00 |
| `0 8 1 * 1` | **POSIX OR** — every 1st **and** every Monday |
| `0 0 1 1 *` | New Year's Day midnight |

## DOM / DOW semantics (LUA-F4 fix, 2026-04-25)

Standard POSIX/Vixie cron (and crontab(5)) special-cases the DOM and
DOW fields when **both** are restricted (non-`*`): the rule fires if
**either** matches. When **either** field is `*` the wildcard is
satisfied automatically and the other side is ANDed.

Implementation in `cron_matches` after the trivial minute / hour /
month checks:

```c
bool dom_match = (expr.day_bits  & (1UL << day))  != 0;
bool dow_match = (expr.wday_bits & (1U  << wday)) != 0;
bool dom_star  = (expr.flags & CRON_FLAG_DOM_STAR) != 0;
bool dow_star  = (expr.flags & CRON_FLAG_DOW_STAR) != 0;

if (dom_star || dow_star) {
    if (!dom_match || !dow_match) return false;   // AND
} else {
    if (!dom_match && !dow_match) return false;   // OR
}
```

Without this fix `0 8 1 * 1` fired only on Mondays that fell on the
1st (~5 days/year) instead of every 1st **and** every Monday
(~64 days/year).

## Threading & concurrency

- Pure functions; no global state, no locks.
- Reentrant; safe from any task or ISR (modulo `localtime_r`'s
  thread-local `tzset` cache).

## Failure modes

| Condition | Behaviour |
|---|---|
| Wrong field count (≠ 5) | `cron_parse` returns false |
| Out-of-range value | `cron_parse` returns false (no partial parse) |
| Step > range | Empty bitmask; `cron_matches` always false |
| `cron_next` finds no match in 4 years | Returns `0` |
| `mktime` failure inside `cron_next` (ambiguous DST) | Returns `0` |

## Integration example

```c
// simple_rules task_cron:
CronExpr expr{};
if (!cron_parse(rule.trigger.key, expr)) {
    ESP_LOGW(TAG, "bad cron in rule %u", rule.rule_id);
    continue;
}
if (cron_matches(expr, now))
    execute_rule(rule, "", nullptr);

// REST endpoint that wants to display the next firing time:
time_t next = cron_next(expr, time(nullptr));
if (next) {
    char buf[32];
    strftime(buf, sizeof(buf), "%F %T", localtime(&next));
    rest_reply_string(buf);
} else {
    rest_reply_error("no match in 4 years");
}
```

## Recent changes

- **2026-04-25 LUA-F4.** DOM/DOW semantics fixed. `flags` byte
  added to `CronExpr` (size 17 → 18 B); `cron_parse` records `*`
  state per field; `cron_matches` and `cron_next` apply
  POSIX-AND-or-OR.
- Earlier: stepped (`*/n`), list (`a,b,c`), and range (`a-b`)
  syntax in every field.

## Cross-references

- `docs/FINDINGS.md` — LUA-F4 (the OR-semantics fix)
- `components/simple_rules/README.md` — TIME_CRON trigger consumer
- `components/lua_engine/README.md` — `zhac.on_cron` consumer
