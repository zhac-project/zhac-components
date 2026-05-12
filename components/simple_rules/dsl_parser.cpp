// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simple_rules.h"
#include "esp_log.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <cstdio>

static const char* TAG = "dsl_parser";

// Last parse-error message, populated at each failure site. Not thread-safe
// but dsl_parse is invoked under simple_rules_add's mutex anyway. Consumed
// by the HAP RULE_CREATE / RULE_UPDATE handlers to surface a specific reason
// in the RULE_EXEC_RESULT err field — otherwise the UI gets the generic
// "parse or store failed" placeholder.
static char s_last_error[96] = "";
static void dsl_set_err(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}
const char* dsl_last_error() { return s_last_error; }

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
    const char* hash = strchr(s, '#');
    t->type = TriggerType::DEVICE_ATTR;
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
        const size_t cap = sizeof(t->device_name) - 1;
        const size_t n   = strnlen(ref, cap);
        memcpy(t->device_name, ref, n);
        t->device_name[n] = '\0';
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
            char* endp = nullptr;
            double d = strtod(v, &endp);
            if (endp == v) {
                ESP_LOGW(TAG, "invalid numeric literal in DSL: %s", v);
                dsl_set_err("invalid numeric literal '%s'", v);
                return ParseResult::ERR_BAD_TRIGGER;
            }
            t->int_val = (int32_t)(d < 0 ? d - 0.5 : d + 0.5);
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

// ── Action parsing ────────────────────────────────────────────────────────

static ParseResult parse_action(const char* s, RuleAction* a) {
    s = skip_ws(s);
    if (strncmp(s, "zigbee.set ", 11) == 0) {
        a->type = ActionType::ZIGBEE_SET;
        s += 11; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        s = skip_ws(s);
        s = copy_token(s, ' ', a->arg1, sizeof(a->arg1));
        s = skip_ws(s);
        strncpy(a->arg2, s, sizeof(a->arg2) - 1);
        a->arg2[sizeof(a->arg2) - 1] = '\0';
        return ParseResult::OK;
    }
    if (strncmp(s, "zigbee.toggle ", 14) == 0) {
        a->type = ActionType::ZIGBEE_TOGGLE;
        s += 14; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        s = skip_ws(s);
        strncpy(a->arg1, s, sizeof(a->arg1) - 1);
        a->arg1[sizeof(a->arg1) - 1] = '\0';
        if (a->arg0[0] == '\0' || a->arg1[0] == '\0') return ParseResult::ERR_BAD_ACTION;
        return ParseResult::OK;
    }
    if (strncmp(s, "publish ", 8) == 0) {
        a->type = ActionType::PUBLISH;
        s += 8; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0));
        s = skip_ws(s);
        strncpy(a->arg1, s, sizeof(a->arg1) - 1);
        a->arg1[sizeof(a->arg1) - 1] = '\0';
        return ParseResult::OK;
    }
    if (strncmp(s, "event ", 6) == 0) {
        a->type = ActionType::EVENT;
        s += 6;
        strncpy(a->arg0, skip_ws(s), sizeof(a->arg0) - 1);
        a->arg0[sizeof(a->arg0) - 1] = '\0';
        return ParseResult::OK;
    }
    if (strncmp(s, "timer ", 6) == 0) {
        a->type = ActionType::TIMER;
        s += 6; s = skip_ws(s);
        s = copy_token(s, ' ', a->arg0, sizeof(a->arg0)); // index
        s = skip_ws(s);
        strncpy(a->arg1, s, sizeof(a->arg1) - 1); // ms
        a->arg1[sizeof(a->arg1) - 1] = '\0';
        return ParseResult::OK;
    }
    if (strncmp(s, "log ", 4) == 0) {
        a->type = ActionType::LOG;
        strncpy(a->arg0, skip_ws(s + 4), sizeof(a->arg0) - 1);
        a->arg0[sizeof(a->arg0) - 1] = '\0';
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
    if (alen >= sizeof(actions_str)) alen = sizeof(actions_str) - 1;
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
