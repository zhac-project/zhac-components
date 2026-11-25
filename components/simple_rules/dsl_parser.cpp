// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simple_rules.h"
#include "esp_log.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstdint>   // INT32_MAX / INT32_MIN bounds for strtod clamp
#include <cmath>     // std::isfinite — reject NaN/inf literals before cast
#include <strings.h> // strcasecmp

static const char* TAG = "dsl_parser";

// Last parse-error message, populated at each failure site. Not thread-safe
// but dsl_parse is invoked under simple_rules_add's mutex anyway. Consumed
// by the HAP RULE_CREATE / RULE_UPDATE handlers to surface a specific reason
// in the RULE_EXEC_RESULT err field — otherwise the UI gets the generic
// "parse or store failed" placeholder.
// 192: a firmware hook's refusal (up to 191 characters) is kept whole.
static char s_last_error[192] = "";
static void dsl_set_err(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}
const char* dsl_last_error() { return s_last_error; }

// Lets simple_rules surface a non-parse failure (cache full, oversize DSL)
// through the SAME channel the HAP RULE_* handlers already read for parse
// errors, so the SPA/cloud sees a specific reason instead of the generic
// "parse or store failed" placeholder. Shares s_last_error (set under the
// simple_rules_add/update mutex, like dsl_parse itself).
void dsl_set_last_error(const char* msg) {
    if (!msg) { s_last_error[0] = '\0'; return; }
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

// ── String helpers ────────────────────────────────────────────────────────

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// Copy token (up to delim or end), null-terminate. Returns pointer after token.
static const char* copy_token(const char* p, char delim, char* out, size_t max) {
    size_t i = 0;
    while (*p && *p != delim && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    // Trim trailing spaces
    while (i > 0 && out[i-1] == ' ') out[--i] = '\0';
    return p;
}

// ── Operator parsing ──────────────────────────────────────────────────────

static CondOp parse_op(const char* s, const char** after) {
    if (s[0]=='<' && s[1]=='=') { *after = s+2; return CondOp::LTE; }
    if (s[0]=='>' && s[1]=='=') { *after = s+2; return CondOp::GTE; }
    if (s[0]=='!' && s[1]=='=') { *after = s+2; return CondOp::NEQ; }
    if (s[0]=='<')              { *after = s+1; return CondOp::LT;  }
    if (s[0]=='>')              { *after = s+1; return CondOp::GT;  }
    if (s[0]=='=')              { *after = s+1; return CondOp::EQ;  }
    *after = s;
    return CondOp::ANY;
}

// ── Trigger parsing ───────────────────────────────────────────────────────

static ParseResult parse_trigger(const char* s, RuleTrigger* t) {
    // System#Boot
    if (strncmp(s, "System#Boot", 11) == 0) {
        t->type = TriggerType::BOOT;
        return ParseResult::OK;
    }
    // Time#Cron=<expr>
    if (strncmp(s, "Time#Cron=", 10) == 0) {
        t->type = TriggerType::TIME_CRON;
        if (strlen(s + 10) >= sizeof(t->key)) {
            dsl_set_err("cron expression too long (max %u)", (unsigned)(sizeof(t->key) - 1));
            return ParseResult::ERR_BAD_TRIGGER;
        }
        strncpy(t->key, s + 10, sizeof(t->key) - 1);
        t->key[sizeof(t->key) - 1] = '\0';
        return ParseResult::OK;
    }
    // Event#<name>
    if (strncmp(s, "Event#", 6) == 0) {
        t->type = TriggerType::EVENT;
        if (strlen(s + 6) >= sizeof(t->key)) {
            dsl_set_err("event name too long (max %u)", (unsigned)(sizeof(t->key) - 1));
            return ParseResult::ERR_BAD_TRIGGER;
        }
        strncpy(t->key, s + 6, sizeof(t->key) - 1);
        t->key[sizeof(t->key) - 1] = '\0';
        return ParseResult::OK;
    }
    // Rules#Timer=<n>
    if (strncmp(s, "Rules#Timer=", 12) == 0) {
        t->type = TriggerType::TIMER;
        if (strlen(s + 12) >= sizeof(t->key)) {
            dsl_set_err("timer ref too long (max %u)", (unsigned)(sizeof(t->key) - 1));
            return ParseResult::ERR_BAD_TRIGGER;
        }
        strncpy(t->key, s + 12, sizeof(t->key) - 1);
        t->key[sizeof(t->key) - 1] = '\0';
        return ParseResult::OK;
    }

    // Mqtt#<topic>
    if (strncmp(s, "Mqtt#", 5) == 0) {
        t->type = TriggerType::MQTT_TOPIC;
        if (strlen(s + 5) >= sizeof(t->key)) {
            dsl_set_err("mqtt topic too long (max %u)", (unsigned)(sizeof(t->key) - 1));
            return ParseResult::ERR_BAD_TRIGGER;
        }
        strncpy(t->key, s + 5, sizeof(t->key) - 1);
        t->key[sizeof(t->key) - 1] = '\0';
        return ParseResult::OK;
    }

    // Device: <ref>[#<key>[<op><value>]]
    // ref can be IEEE (0xHEX...) or friendly name.
    // When `#<key>` is omitted, attr_key stays empty — the matcher treats
    // that as "match any attribute change on this device" and the script
    // hook receives the full event table so a Lua handler can inspect
    // `ev.key` / `ev.value` itself.
    // Offline#<ref>: a mains device stopped answering its availability ping (research 2026-11-18 B7). The ref
    // reads as below; there is no #<key>.
    const bool offline = strncmp(s, "Offline#", 8) == 0;
    if (offline) s += 8;
    const char* hash = strchr(s, '#');
    if (offline && hash) {
        dsl_set_err("Offline# takes a device only, no #attribute");
        return ParseResult::ERR_BAD_TRIGGER;
    }
    t->type = offline ? TriggerType::DEVICE_OFFLINE : TriggerType::DEVICE_ATTR;
    char ref[64]{};
    size_t ref_len = hash ? (size_t)(hash - s) : strlen(s);
    // Trim trailing whitespace from the device ref when there's no `#`.
    while (ref_len > 0 && (s[ref_len - 1] == ' ' || s[ref_len - 1] == '\t'))
        ref_len--;
    if (ref_len == 0 || ref_len >= sizeof(ref)) {
        dsl_set_err("device ref empty or too long (max %u)", (unsigned)(sizeof(ref) - 1));
        return ParseResult::ERR_BAD_TRIGGER;
    }
    memcpy(ref, s, ref_len);

    // Check if ref is IEEE address. Reject a malformed 0x literal instead
    // of letting strtoull silently degrade to 0 — ieee == 0 is treated as
    // "wildcard" downstream, so `ON 0xGARBAGE#state=1` would otherwise
    // match every device.
    if (ref[0]=='0' && (ref[1]=='x'||ref[1]=='X')) {
        char* endp = nullptr;
        uint64_t parsed = (uint64_t)strtoull(ref, &endp, 16);
        if (!endp || endp == ref || *endp != '\0' || parsed == 0) {
            dsl_set_err("invalid IEEE literal '%s'", ref);
            return ParseResult::ERR_BAD_TRIGGER;
        }
        t->ieee = parsed;
    } else {
        t->ieee = 0; // will be resolved by simple_rules_resolve_names
        // A longer name can never equal a device's friendly_name, so fail the
        // parse instead of storing a rule that silently never fires.
        if (ref_len >= sizeof(t->device_name)) {
            dsl_set_err("device name too long (max %u)", (unsigned)(sizeof(t->device_name) - 1));
            return ParseResult::ERR_BAD_TRIGGER;
        }
        memcpy(t->device_name, ref, ref_len);
        t->device_name[ref_len] = '\0';
    }

    if (!hash) {
        // No `#attr` — wildcard form. attr_key stays empty; op is ANY.
        t->attr_key[0] = '\0';
        t->op = CondOp::ANY;
        return ParseResult::OK;
    }
    const char* rest = hash + 1;
    // Find operator position
    const char* op_pos = rest;
    while (*op_pos && *op_pos != '=' && *op_pos != '!' &&
           *op_pos != '<' && *op_pos != '>') op_pos++;

    // Copy attr key (between # and operator) into the trigger's string slot.
    size_t klen = (size_t)(op_pos - rest);
    if (klen == 0 || klen >= ATTR_KEY_MAX) {
        dsl_set_err("attr key empty or too long (max %u)", (unsigned)(ATTR_KEY_MAX - 1));
        return ParseResult::ERR_BAD_TRIGGER;
    }
    memcpy(t->attr_key, rest, klen);
    t->attr_key[klen] = '\0';

    if (*op_pos) {
        const char* after_op = nullptr;
        t->op = parse_op(op_pos, &after_op);
        strncpy(t->value, after_op, sizeof(t->value) - 1);
        t->value[sizeof(t->value) - 1] = '\0';

        const char* v = after_op;
        while (*v == ' ' || *v == '\t') v++;

        if (*v == '"') {
            // Quoted string literal — stored verbatim.
            const char* start = ++v;
            const char* end = start;
            while (*end && *end != '"') end++;
            if (*end != '"') {
                ESP_LOGW(TAG, "unterminated string literal in DSL");
                dsl_set_err("unterminated string literal");
                return ParseResult::ERR_BAD_TRIGGER;
            }
            size_t slen = (size_t)(end - start);
            if (slen >= ATTR_STR_MAX) {
                dsl_set_err("string literal too long (max %u)", ATTR_STR_MAX - 1);
                return ParseResult::ERR_BAD_TRIGGER;
            }
            memcpy(t->str_val, start, slen);
            t->str_val[slen] = '\0';
            t->match_val_type = VAL_STR;
            t->int_val = 0;
        } else {
            // P2-T18 def 5 (FINDINGS §7): a raw strtod→int32_t cast is UB for
            // out-of-range literals (e.g. `#temp>1e20` from a REST/cloud rule).
            // Check errno==ERANGE and the INT32 bounds BEFORE rounding/casting.
            char* endp = nullptr;
            errno = 0;
            double d = strtod(v, &endp);
            // A number with a decimal point (`>25.5`) is in real units: held
            // ×100, the scale of a VAL_FLOAT reading, and flagged so the matcher
            // lifts an INT/BOOL reading to meet it. One without (`>2500`) keeps
            // the raw meaning it always had, so saved rules match as before.
            // A '.' in what strtod read implies a finite or overflowing value
            // (never NaN), so a decimal literal fails below only out of range.
            const bool x100 = endp != v && memchr(v, '.', (size_t)(endp - v)) != nullptr;
            if (x100) d *= 100.0;
            // Bound the ROUNDED magnitude: round-half-away-from-zero can push
            // a value up to 0.5 past d, so check (|d| + 0.5) against the
            // int32 limits. This both keeps the subsequent (int32_t) cast in
            // defined range and rejects out-of-range literals like 1e20.
            //
            // NaN must be rejected EXPLICITLY: `nan >= X` and `nan <= Y` are
            // both false, so a `nan` literal (strtod parses it) would slip the
            // magnitude guard and reach `(int32_t)nan` = UB. !std::isfinite(d)
            // catches NaN AND inf in one check; the magnitude bounds below are
            // then redundant for inf but KEPT (defensive + clear). isfinite is
            // ordered first so non-finite values short-circuit before compare.
            if (endp == v || errno == ERANGE || !std::isfinite(d) ||
                d >= (double)INT32_MAX + 0.5 || d <= (double)INT32_MIN - 0.5) {
                ESP_LOGW(TAG, "invalid/out-of-range numeric literal in DSL: %s", v);
                if (x100) dsl_set_err("decimal literal '%s' out of range (max +/-21474836.47)", v);
                else      dsl_set_err("invalid numeric literal '%s'", v);
                return ParseResult::ERR_BAD_TRIGGER;
            }
            double r = (d < 0 ? d - 0.5 : d + 0.5);
            // Clamp the rounded result to the representable range before the
            // cast — guards the exact-boundary case (e.g. INT32_MAX + 0.5).
            if (r > (double)INT32_MAX) r = (double)INT32_MAX;
            else if (r < (double)INT32_MIN) r = (double)INT32_MIN;
            t->int_val = (int32_t)r;
            t->lit_x100 = x100;
            t->match_val_type = VAL_INT;
            t->str_val[0] = '\0';
        }
    } else {
        t->op = CondOp::ANY;
        t->match_val_type = VAL_INT;
        t->int_val = 0;
        t->str_val[0] = '\0';
    }
    return ParseResult::OK;
}

// ── Tier-2 value argument (zigbee.set / publish) ──────────────────────────
// The VALUE is the rest of the action segment. If it contains %value% beyond
// the bare passthrough, it is compiled ONCE here as an expression (spaces
// allowed inside — see expr_eval.h); otherwise the FIRST token is copied into
// `dst` with the legacy copy_token semantics, so existing literal / bare
// %value% rules stay byte-identical.
// copy_token stops silently at its buffer: a 24-char attribute key in a
// 20-byte field used to leave "point 21.5" as the NEXT token, so the value
// became the key's tail and nothing said so. True when a token did not fit.
static bool token_overflowed(const char* after) { return *after != '\0' && *after != ' '; }

static ParseResult parse_value_arg(const char* s, RuleAction* a,
                                   char* dst, size_t dst_cap) {
    s = skip_ws(s);
    size_t tlen = strlen(s);
    while (tlen > 0 && (s[tlen - 1] == ' ' || s[tlen - 1] == '\t')) tlen--;

    // Probe window is deliberately > EXPR_TEXT_MAX so an over-long expression
    // is DETECTED and rejected instead of silently truncating into a
    // different-but-parseable one (the P2-T18 clamp lesson).
    char tail[128];
    const size_t probe = (tlen < sizeof(tail) - 1) ? tlen : sizeof(tail) - 1;
    memcpy(tail, s, probe);
    tail[probe] = '\0';

    if (strstr(tail, "%value%") != nullptr && strcmp(tail, "%value%") != 0) {
        if (tlen > EXPR_TEXT_MAX) {
            dsl_set_err("value expression too long (max %u)",
                        (unsigned)EXPR_TEXT_MAX);
            return ParseResult::ERR_BAD_ACTION;
        }
        char err[64] = {};
        if (!expr_compile(tail, a->expr, err, sizeof err)) {
            dsl_set_err("bad value expression: %s", err);
            return ParseResult::ERR_BAD_ACTION;
        }
        a->has_expr = true;
        // Truncated copy for display/debug (the compiled expression is what
        // runs). Bounded memcpy: the truncation is the point, and both
        // strncpy and snprintf make -Werror=*-truncation call it a bug.
        const size_t tl = strlen(tail);
        const size_t n  = tl < dst_cap - 1 ? tl : dst_cap - 1;
        memcpy(dst, tail, n);
        dst[n] = '\0';
        return ParseResult::OK;
    }
    const char* after = copy_token(s, ' ', dst, dst_cap);
    if (token_overflowed(after)) {
        dsl_set_err("value too long (max %u)", (unsigned)(dst_cap - 1));
        return ParseResult::ERR_BAD_ACTION;
    }
    return ParseResult::OK;
}

// ── Hub pins: gpio.set / gpio.toggle / pwm.set ───────────────────────────

static simple_rules_io_check_t s_io_check = nullptr;
void dsl_set_io_check(simple_rules_io_check_t check) { s_io_check = check; }

// on/off/1/0/true/false/high/low, any case: the literal values gpio.set takes.
static bool is_level_word(const char* v) {
    static constexpr const char* kWords[] = {"on", "off", "1", "0", "true", "false", "high", "low"};
    for (const char* w : kWords) if (strcasecmp(v, w) == 0) return true;
    return false;
}

// A literal duty: a number 0..100, decimals allowed, nothing after it.
static bool is_duty(const char* v) {
    char* end = nullptr;
    const double d = strtod(v, &end);
    return end != v && *end == '\0' && std::isfinite(d) && d >= 0.0 && d <= 100.0;
}

// The pin token, then the firmware's check of it. `s` is left after the token.
static ParseResult parse_pin(const char*& s, RuleAction* a, SimpleRulesIoOp op, const char* verb) {
    s = copy_token(skip_ws(s), ' ', a->arg0, sizeof(a->arg0));
    if (token_overflowed(s)) {
        dsl_set_err("%s: pin name too long (max %u)", verb, (unsigned)(sizeof(a->arg0) - 1));
        return ParseResult::ERR_BAD_ACTION;
    }
    if (a->arg0[0] == '\0') { dsl_set_err("%s: pin missing", verb); return ParseResult::ERR_BAD_ACTION; }
    char err[96] = {};
    if (s_io_check && !s_io_check(op, a->arg0, err, sizeof(err))) {
        dsl_set_err("%s", err[0] ? err : "unknown pin");
        return ParseResult::ERR_BAD_ACTION;
    }
    return ParseResult::OK;
}

// gpio.set / pwm.set: pin, then a literal of the verb's kind, %value% or an expression.
static ParseResult parse_pin_value(const char* s, RuleAction* a, SimpleRulesIoOp op, const char* verb) {
    ParseResult r = parse_pin(s, a, op, verb);
    if (r != ParseResult::OK) return r;
    r = parse_value_arg(s, a, a->arg2, sizeof(a->arg2));
    if (r != ParseResult::OK || a->has_expr) return r;
    if (a->arg2[0] == '\0') { dsl_set_err("%s: value missing", verb); return ParseResult::ERR_BAD_ACTION; }
    if (strcmp(a->arg2, "%value%") == 0) return ParseResult::OK;
    if (op == SimpleRulesIoOp::PWM ? !is_duty(a->arg2) : !is_level_word(a->arg2)) {
        dsl_set_err(op == SimpleRulesIoOp::PWM ? "%s: duty must be 0-100, not '%s'"
                                               : "%s: value must be on/off/1/0/true/false/high/low, not '%s'",
                    verb, a->arg2);
        return ParseResult::ERR_BAD_ACTION;
    }
    return ParseResult::OK;
}

// ── Strip and sound: strip.set / strip.off / sound.play / sound.stop ─────

static simple_rules_sound_check_t s_sound_check = nullptr;
void dsl_set_sound_check(simple_rules_sound_check_t check) { s_sound_check = check; }

// Next space-delimited token of an action tail into `out`. False: none left. `*fit` false: it was
// longer than `cap` - 1 (`out` holds its head).
static bool next_token(const char*& s, char* out, size_t cap, bool* fit) {
    s = copy_token(skip_ws(s), ' ', out, cap);
    *fit = !token_overflowed(s);
    return out[0] != '\0';
}

// '#' and six hex digits, any case.
static bool is_colour(const char* v) {
    if (v[0] != '#' || strlen(v) != 7) return false;
    for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)v[i])) return false;
    return true;
}

// 0..100 as one to three decimal digits: no sign, no decimals, no suffix.
static bool is_percent(const char* v) {
    const size_t n = strlen(v);
    if (n == 0 || n > 3) return false;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)v[i])) return false;
    return atoi(v) <= 100;
}

// strip.set <pin> <#RRGGBB> [<0-100>] / strip.off <pin>. Stored: arg0 pin, arg2 the colour, then the
// brightness when given, joined by one space (what the run hook gets); strip.off stores "#000000".
static ParseResult parse_strip(const char* s, RuleAction* a, bool set) {
    const char* verb = set ? "strip.set" : "strip.off";
    ParseResult r = parse_pin(s, a, SimpleRulesIoOp::STRIP, verb);
    if (r != ParseResult::OK) return r;
    if (!set) {
        if (*skip_ws(s) != '\0') { dsl_set_err("strip.off takes one pin"); return ParseResult::ERR_BAD_ACTION; }
        strcpy(a->arg2, "#000000");
        return ParseResult::OK;
    }
    char col[24], t[24];
    bool fit;
    if (!next_token(s, col, sizeof(col), &fit)) { dsl_set_err("strip.set: colour missing"); return ParseResult::ERR_BAD_ACTION; }
    if (!fit || !is_colour(col)) { dsl_set_err("strip.set: colour must be #RRGGBB"); return ParseResult::ERR_BAD_ACTION; }
    if (next_token(s, t, sizeof(t), &fit)) {
        if (!fit || !is_percent(t)) { dsl_set_err("strip.set: brightness must be 0-100"); return ParseResult::ERR_BAD_ACTION; }
        snprintf(a->arg2, sizeof(a->arg2), "%.7s %.3s", col, t);   // both validated: 7 and 1-3 characters
        if (next_token(s, t, sizeof(t), &fit)) { dsl_set_err("strip.set: unexpected '%s'", t); return ParseResult::ERR_BAD_ACTION; }
    } else {
        snprintf(a->arg2, sizeof(a->arg2), "%.7s", col);
    }
    return ParseResult::OK;
}

// Calls the firmware's sound check and keeps its words. The only function with the 192-byte buffer,
// and noinline so it stays out of its callers' frames: it is entered only when a check is set, which
// the boot reload (on a small stack) never does.
__attribute__((noinline)) static ParseResult run_sound_check(const char* name, bool loop) {
    char err[192] = {};
    if (s_sound_check(name, loop, err, sizeof(err))) return ParseResult::OK;
    dsl_set_err("%s", err[0] ? err : "unknown sound");
    return ParseResult::ERR_BAD_ACTION;
}

// The reload parses every stored sound rule through here with no check set: it returns before any buffer.
static ParseResult check_sound(const char* name, bool loop) {
    if (!s_sound_check) return ParseResult::OK;
    return run_sound_check(name, loop);
}

// sound.play <name> [<0-100>] [loop] / sound.stop [<name>]. Stored: arg0 name ("" for a bare stop),
// arg1 the volume text or "", arg2 "loop" or "".
static ParseResult parse_sound(const char* s, RuleAction* a, bool play) {
    const char* verb = play ? "sound.play" : "sound.stop";
    char t[24];
    bool fit;
    const bool named = next_token(s, a->arg0, sizeof(a->arg0), &fit);
    if (!fit) { dsl_set_err("%s: sound name too long (max %u)", verb, (unsigned)(sizeof(a->arg0) - 1)); return ParseResult::ERR_BAD_ACTION; }
    if (!play) {
        if (next_token(s, t, sizeof(t), &fit)) { dsl_set_err("sound.stop takes at most one name"); return ParseResult::ERR_BAD_ACTION; }
        return check_sound(a->arg0, false);
    }
    if (!named) { dsl_set_err("sound.play: sound name missing"); return ParseResult::ERR_BAD_ACTION; }
    bool more = next_token(s, t, sizeof(t), &fit);
    if (more && (!fit || strcmp(t, "loop") != 0)) {   // a volume
        if (!fit || !is_percent(t)) { dsl_set_err("sound.play: volume must be 0-100"); return ParseResult::ERR_BAD_ACTION; }
        snprintf(a->arg1, sizeof(a->arg1), "%s", t);
        more = next_token(s, t, sizeof(t), &fit);
    }
    if (more && fit && strcmp(t, "loop") == 0) {
        strcpy(a->arg2, "loop");
        more = next_token(s, t, sizeof(t), &fit);
    }
    if (more) { dsl_set_err("sound.play: unexpected '%s'", t); return ParseResult::ERR_BAD_ACTION; }
    return check_sound(a->arg0, a->arg2[0] != '\0');
}

// ── Action parsing ────────────────────────────────────────────────────────

static ParseResult parse_action(const char* s, RuleAction* a) {
    s = skip_ws(s);
    if (strncmp(s, "gpio.set ", 9) == 0) {
        a->type = ActionType::GPIO_SET;
        return parse_pin_value(s + 9, a, SimpleRulesIoOp::SET, "gpio.set");
    }
    if (strncmp(s, "pwm.set ", 8) == 0) {
        a->type = ActionType::PWM_SET;
        return parse_pin_value(s + 8, a, SimpleRulesIoOp::PWM, "pwm.set");
    }
    if (strncmp(s, "gpio.toggle", 11) == 0 && (s[11] == ' ' || s[11] == '\0')) {
        a->type = ActionType::GPIO_TOGGLE;
        s += 11;
        const ParseResult r = parse_pin(s, a, SimpleRulesIoOp::TOGGLE, "gpio.toggle");
        if (r != ParseResult::OK) return r;
        if (*skip_ws(s) != '\0') { dsl_set_err("gpio.toggle takes one pin"); return ParseResult::ERR_BAD_ACTION; }
        return ParseResult::OK;
    }
    if (strncmp(s, "strip.set", 9) == 0 && (s[9] == ' ' || s[9] == '\0')) {
        a->type = ActionType::STRIP_SET;
        return parse_strip(s + 9, a, true);
    }
    if (strncmp(s, "strip.off", 9) == 0 && (s[9] == ' ' || s[9] == '\0')) {
        a->type = ActionType::STRIP_OFF;
        return parse_strip(s + 9, a, false);
    }
    if (strncmp(s, "sound.play", 10) == 0 && (s[10] == ' ' || s[10] == '\0')) {
        a->type = ActionType::SOUND_PLAY;
        return parse_sound(s + 10, a, true);
    }
    if (strncmp(s, "sound.stop", 10) == 0 && (s[10] == ' ' || s[10] == '\0')) {
        a->type = ActionType::SOUND_STOP;
        return parse_sound(s + 10, a, false);
    }
    // Tail-arg note: actions are extracted as the substring between
    // `DO ` and `ENDON`, so the last action's last argument keeps the
    // trailing space before ENDON (e.g. `zigbee.toggle dev state `).
    // Using copy_token (with delim=' ', which also rtrims) for single-
    // token args strips that space and prevents lookups like
    // `attr 'state '` from failing in the runtime.
    if (strncmp(s, "zigbee.set ", 11) == 0) {
        a->type = ActionType::ZIGBEE_SET;
        s += 11; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        if (token_overflowed(s)) {
            dsl_set_err("device ref too long (max %u)", (unsigned)(sizeof(a->arg0) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        s = skip_ws(s);
        s = copy_token(s, ' ', a->arg1, sizeof(a->arg1));
        if (token_overflowed(s)) {
            dsl_set_err("attr key too long (max %u)", (unsigned)(sizeof(a->arg1) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        return parse_value_arg(s, a, a->arg2, sizeof(a->arg2));
    }
    if (strncmp(s, "zigbee.toggle ", 14) == 0) {
        a->type = ActionType::ZIGBEE_TOGGLE;
        s += 14; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        if (token_overflowed(s)) {
            dsl_set_err("device ref too long (max %u)", (unsigned)(sizeof(a->arg0) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        s = skip_ws(s);
        s = copy_token(s, ' ', a->arg1, sizeof(a->arg1));
        if (token_overflowed(s)) {
            dsl_set_err("attr key too long (max %u)", (unsigned)(sizeof(a->arg1) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        if (a->arg0[0] == '\0' || a->arg1[0] == '\0') return ParseResult::ERR_BAD_ACTION;
        return ParseResult::OK;
    }
    if (strncmp(s, "publish ", 8) == 0) {
        a->type = ActionType::PUBLISH;
        s += 8; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        if (token_overflowed(s)) {
            dsl_set_err("topic too long (max %u)", (unsigned)(sizeof(a->arg0) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        return parse_value_arg(s, a, a->arg1, sizeof(a->arg1));
    }
    if (strncmp(s, "event ", 6) == 0) {
        a->type = ActionType::EVENT;
        s += 6; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        if (token_overflowed(s)) {
            dsl_set_err("event name too long (max %u)", (unsigned)(sizeof(a->arg0) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        return ParseResult::OK;
    }
    if (strncmp(s, "timer ", 6) == 0) {
        a->type = ActionType::TIMER;
        s += 6; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0)); // index
        if (token_overflowed(s)) { dsl_set_err("timer index too long"); return ParseResult::ERR_BAD_ACTION; }
        s = skip_ws(s);
        s = copy_token(s, ' ', a->arg1, sizeof(a->arg1)); // ms
        if (token_overflowed(s)) { dsl_set_err("timer duration too long"); return ParseResult::ERR_BAD_ACTION; }
        return ParseResult::OK;
    }
    if (strncmp(s, "log ", 4) == 0) {
        // log message is the only multi-word tail; keep rest-of-string
        // semantics but strip the trailing space inserted before ENDON.
        // The ONE place a long token is still cut rather than refused: the
        // text only reaches the log, so a rule saved on an older build must
        // keep loading at boot instead of vanishing over a long message.
        a->type = ActionType::LOG;
        strncpy(a->arg0, skip_ws(s + 4), sizeof(a->arg0) - 1);
        a->arg0[sizeof(a->arg0) - 1] = '\0';
        size_t n = strlen(a->arg0);
        while (n > 0 && (a->arg0[n-1] == ' ' || a->arg0[n-1] == '\t'))
            a->arg0[--n] = '\0';
        return ParseResult::OK;
    }
    if (strncmp(s, "script.run ", 11) == 0) {
        a->type = ActionType::SCRIPT;
        s += 11; s = skip_ws(s);
        // Script name: accept quoted ("motion") or bare (motion).
        // Length limited by arg0 buffer; the Lua engine applies the
        // canonical name-sanitize on write separately.
        size_t i = 0;
        if (*s == '"') {
            s++;
            while (*s && *s != '"' && i < sizeof(a->arg0) - 1) {
                a->arg0[i++] = *s++;
            }
        } else {
            while (*s && *s != ' ' && *s != '\t' && i < sizeof(a->arg0) - 1) {
                a->arg0[i++] = *s++;
            }
        }
        a->arg0[i] = '\0';
        if (i == 0) return ParseResult::ERR_BAD_ACTION;
        // A name that did not fit would run some other (or no) script.
        if (*s != '\0' && *s != '"' && *s != ' ' && *s != '\t') {
            dsl_set_err("script name too long (max %u)", (unsigned)(sizeof(a->arg0) - 1));
            return ParseResult::ERR_BAD_ACTION;
        }
        return ParseResult::OK;
    }
    return ParseResult::ERR_BAD_ACTION;
}

// ── Main entry ────────────────────────────────────────────────────────────

ParseResult dsl_parse(const char* dsl, uint16_t rule_id, ParsedRule* out) {
    memset(out, 0, sizeof(ParsedRule));
    out->rule_id = rule_id;
    out->enabled = true;

    const char* p = skip_ws(dsl);
    if (strncmp(p, "ON ", 3) != 0) return ParseResult::ERR_NO_ON;
    p += 3; p = skip_ws(p);

    // Find " DO "
    const char* do_pos = strstr(p, " DO ");
    if (!do_pos) return ParseResult::ERR_NO_DO;

    // Parse trigger string (between ON and DO)
    char trigger_str[128]{};
    size_t tlen = (size_t)(do_pos - p);
    if (tlen >= sizeof(trigger_str)) return ParseResult::ERR_BAD_TRIGGER;
    memcpy(trigger_str, p, tlen);
    ParseResult res = parse_trigger(trigger_str, &out->trigger);
    if (res != ParseResult::OK) return res;

    // Find ENDON
    const char* action_start = do_pos + 4;
    const char* endon = strstr(action_start, "ENDON");
    if (!endon) return ParseResult::ERR_NO_ENDON;

    // Split actions by ';'
    char actions_str[500]{};
    size_t alen = (size_t)(endon - action_start);
    // P2-T18 def 4 (FINDINGS §7): reject instead of silently clamping to 499.
    // A clamp parsed a truncated (different) action set than the rule the
    // caller submitted / the slot persists, so the live and stored rules
    // diverged. Surface a real error.
    if (alen >= sizeof(actions_str)) {
        dsl_set_err("action section too long (max %u)",
                    (unsigned)(sizeof(actions_str) - 1));
        return ParseResult::ERR_ACTION_TOO_LONG;
    }
    memcpy(actions_str, action_start, alen);

    char* tok = actions_str;
    while (tok && out->action_count < 4) {
        char* semi = strchr(tok, ';');
        if (semi) *semi = '\0';
        res = parse_action(tok, &out->actions[out->action_count]);
        if (res != ParseResult::OK) return res;
        out->action_count++;
        tok = semi ? semi + 1 : nullptr;
    }
    // Check if there are more actions beyond the limit of 4
    if (tok && skip_ws(tok)[0] != '\0') return ParseResult::ERR_TOO_MANY_ACTIONS;
    if (out->action_count == 0) return ParseResult::ERR_BAD_ACTION;

    return ParseResult::OK;
}
