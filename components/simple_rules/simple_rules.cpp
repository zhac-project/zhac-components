// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simple_rules.h"
#include "zap_clock.h"
#include "rule_store.h"
#include "esp_heap_caps.h"
#include "event_bus.h"
#include "mqtt_gw.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"
#include "device_shadow.h"
#include "cron_parser.h"
#include "esp_log.h"
#include "device_cmd.h"
#include <cerrno>    // strtol range check in resolve_action_value
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "zhac_task.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cinttypes>
#include <time.h>    // clock_gettime(CLOCK_MONOTONIC): last-run times survive the clock being set
#include "task_stacks.h"

static const char* TAG = "simple_rules";

// TTL for rule→rule event chains. Replaces the old MAX_DISPATCH_DEPTH
// counter, which was dead code: rule-event delivery is queue-based, so a
// self-feeding rule (`ON Event#x DO event x ENDON`) re-enqueues into the
// queue event_bus_drain is draining and every hop arrives as a fresh
// dispatch_event call at depth 0 — the drain never returned and the P4
// main loop wedged until the watchdog rebooted (with the rule persisted,
// it re-wedged every boot). Instead each RULE_EVENT payload carries a hop
// counter; the EVENT action drops the republish once the chain is this long.
static constexpr uint8_t MAX_EVENT_HOPS = 8;

// ── In-memory rule cache ──────────────────────────────────────────────────

static constexpr uint16_t MAX_CACHED_RULES = 64;
static ParsedRule*         s_rules      = nullptr; // allocated in PSRAM on init
static uint16_t            s_rule_count = 0;
static SemaphoreHandle_t   s_mutex = nullptr; // recursive

// F27 (FINDINGS.md): per-slot CronExpr cache so task_cron doesn't re-parse
// every TIME_CRON rule's string every second. Slot-indexed; invalidated
// wholesale on any rule-list mutation (add/update/delete/reload) — delete
// compaction shifts the slot→rule mapping, so a blanket reset is the safe
// choice. enable() doesn't change the key/slot, so it needs no invalidation.
// All access is under s_mutex (task_cron holds it during the collect loop).
enum : uint8_t { CRON_UNCACHED = 0, CRON_BAD = 1, CRON_GOOD = 2 };
static CronExpr s_cron_cache[MAX_CACHED_RULES];
static uint8_t  s_cron_state[MAX_CACHED_RULES] = {};
static inline void cron_cache_invalidate() {
    memset(s_cron_state, 0, sizeof(s_cron_state));
}

// Optional error callback — registered from zhac-main-core/main.cpp after HAP is up
static rules_error_cb_t s_error_cb = nullptr;

// Script hook — registered by lua_engine_init so the SCRIPT action can
// fire a Lua script without simple_rules taking a hard dependency on
// the engine.
static simple_rules_script_hook_t s_script_hook = nullptr;

void simple_rules_set_script_hook(simple_rules_script_hook_t hook) {
    s_script_hook = hook;
}

void simple_rules_set_error_cb(rules_error_cb_t cb) { s_error_cb = cb; }

// ── Software timers (user indices 1–8 → array indices 0–7) ───────────────

static TimerHandle_t s_timers[8] = {};

static void timer_cb(TimerHandle_t xTimer) {
    uint8_t idx = (uint8_t)(uintptr_t)pvTimerGetTimerID(xTimer);
    Event ev{};
    ev.type = EventType::RULE_TIMER_FIRE;
    auto& p = *reinterpret_cast<RuleTimerPayload*>(ev.data);
    p.timer_index = idx;
    event_bus_publish(ev);
}

// ── Helpers ───────────────────────────────────────────────────────────────

static uint16_t next_rule_id() {
    // P2-T18 def 1 (FINDINGS §7): derive the id from the ENTIRE persisted
    // store, not just the 64-entry in-memory cache. The cache holds at most
    // MAX_CACHED_RULES of up to ZAP_MAX_RULES (256) persisted rules; a
    // persisted-but-uncached rule is invisible to a cache-only scan, so its
    // id could be reissued here and the deferred NVS write would then
    // silently overwrite it (permanent data loss). rule_store_max_id()
    // walks all NVS keys + the writeback overlay.
    uint16_t max_id = rule_store_max_id();
    for (uint16_t i = 0; i < s_rule_count; i++)
        if (s_rules[i].rule_id > max_id) max_id = s_rules[i].rule_id;
    // F25 (FINDINGS.md): max_id+1 wraps to 0 at 0xFFFF and could collide with
    // an existing rule. Fast path while ids are small; otherwise scan for the
    // lowest free id (always exists — rule count is capped far below 65535).
    if (max_id < 0xFFFF) return static_cast<uint16_t>(max_id + 1);
    for (uint16_t cand = 1; cand != 0; cand++) {
        bool used = false;
        for (uint16_t i = 0; i < s_rule_count; i++)
            if (s_rules[i].rule_id == cand) { used = true; break; }
        if (used) continue;
        // A free cache slot doesn't prove the id is free in the store.
        RuleSlot probe{};
        if (!rule_store_load(cand, &probe)) return cand;
    }
    return 0;
}

// Regression guard for the 2026-07-09 boot stack overflow (main task = 3584 B).
// ParsedRule carries 4 RuleActions, each embedding an ExprProg, so it is large.
// reload_locked() keeps it OFF the stack by parsing into the PSRAM pool; the
// fire-path snapshots (dispatch_event / task_cron) keep a stack copy but run on
// 8 KB task stacks (kEventBus / kRuleCron). This cap trips if the struct
// balloons past what those task stacks can safely hold as a local — recheck
// both the pool-parse and the snapshot paths if it ever fires.
static_assert(sizeof(ParsedRule) <= 2048,
              "ParsedRule too large — see the reload_locked / fire-path stack note");

static void seed(ParsedRule& r, const ParsedRule* prev);   // Edge triggers, below

static void reload_locked() {
    // F47 (FINDINGS.md): ~34 KB scratch — allocate from PSRAM (internal DRAM is
    // the tight pool on this device), falling back to internal heap on failure.
    auto* slots = static_cast<RuleSlot*>(
        heap_caps_malloc(sizeof(RuleSlot) * MAX_CACHED_RULES, MALLOC_CAP_SPIRAM));
    if (!slots) slots = static_cast<RuleSlot*>(malloc(sizeof(RuleSlot) * MAX_CACHED_RULES));
    // Parse into a fresh pool so each rule can take its run counters and trigger
    // memory over from the one it replaces: a device rename reloads every rule,
    // and that must neither reset "Last ran" nor re-fire anything. On failure
    // the old rules stay active.
    auto* fresh = static_cast<ParsedRule*>(
        heap_caps_calloc(MAX_CACHED_RULES, sizeof(ParsedRule), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slots || !fresh) { free(slots); free(fresh); ESP_LOGE(TAG, "reload: malloc failed"); return; }
    uint16_t cnt = rule_store_load_all(slots, MAX_CACHED_RULES);
    uint16_t n = 0;
    for (uint16_t i = 0; i < cnt && n < MAX_CACHED_RULES; i++) {
        if (slots[i].rule_type != (uint8_t)RuleType::SIMPLE) continue;
        // Parse straight into the PSRAM pool slot. ParsedRule is ~0.9 KB — each
        // of its 4 RuleActions embeds an ExprProg (Tier-2 %value% expressions) —
        // and this reload runs on the 3584-byte main task at boot
        // (simple_rules_init/reload). A stack-local ParsedRule here overflowed it
        // once dsl_parse's own frame (parse_value_arg probe + expr_compile) sat
        // on top. The slot is not committed until n is bumped, so a parse
        // failure just leaves it for the next iteration (dsl_parse zeroes it).
        ParsedRule* r = &fresh[n];
        ParseResult res = dsl_parse((const char*)slots[i].src, slots[i].rule_id, r);
        if (res != ParseResult::OK) {
            ESP_LOGW(TAG, "rule %u parse error %d — skipped", slots[i].rule_id, (int)res);
            if (s_error_cb) {
                static char err_msg[48];
                snprintf(err_msg, sizeof(err_msg), "parse error %d", (int)res);
                s_error_cb(slots[i].rule_id, err_msg);
            }
            continue;
        }
        r->enabled = slots[i].enabled != 0;
        snprintf(r->name, sizeof(r->name), "%.*s", (int)sizeof(r->name) - 1, slots[i].name);
        n++;
    }
    simple_rules_resolve_names(fresh, n);
    for (uint16_t i = 0; i < n; i++) {
        const ParsedRule* old = nullptr;
        for (uint16_t j = 0; j < s_rule_count && !old; j++)
            if (s_rules[j].rule_id == fresh[i].rule_id) old = &s_rules[j];
        if (old) fresh[i].stat = old->stat;
        seed(fresh[i], old);
    }
    free(s_rules);
    s_rules = fresh;
    s_rule_count = n;
    cron_cache_invalidate();   // F27: rules reloaded — drop stale cron cache
    free(slots);

    // P2-T18 def 3 (FINDINGS §7): rule_store_load_all caps at
    // MAX_CACHED_RULES, so when more rules are persisted than the active
    // cache holds, an arbitrary subset is silently dropped (never evaluated).
    // We deliberately keep the 64-active cap (raising to 256 = +~130 KB P4
    // DRAM) but must not be silent about it — warn with the skipped count so
    // operators know rules above the cap are inert. See README "Active-rule
    // limit".
    uint16_t persisted = rule_store_count();
    if (persisted > s_rule_count) {
        ESP_LOGW(TAG,
                 "%u persisted rules but only %u active (cap=%u): %u rule(s) "
                 "are stored yet NOT evaluated — reduce rule count or raise "
                 "MAX_CACHED_RULES",
                 persisted, s_rule_count, MAX_CACHED_RULES,
                 (unsigned)(persisted - s_rule_count));
    }
}

// Expand %value% in src into dst using the supplied substitution string.
static void expand_value(const char* src, const char* val,
                         char* dst, size_t dst_size) {
    const char* needle = "%value%";
    const char* p = strstr(src, needle);
    if (!p) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }
    size_t before = (size_t)(p - src);
    snprintf(dst, dst_size, "%.*s%s%s",
             (int)before, src, val, p + strlen(needle));
}

// Tier-2: resolve an action VALUE — a compiled %value% expression or the
// legacy expand_value path. Returns false when the action must be SKIPPED
// (non-numeric trigger value feeding a numeric expression, or a runtime
// division/modulo by zero); a skipped action sends nothing and writes no
// optimistic shadow value.
static bool resolve_action_value(const RuleAction& a, const char* legacy_arg,
                                 const char* event_val,
                                 char* out, size_t out_size) {
    if (!a.has_expr) {
        expand_value(legacy_arg, event_val, out, out_size);
        return true;
    }
    // strtoll, not strtol: long is 32-bit on the ESP32 targets, which would
    // both truncate and make the range guard an always-false comparison
    // (-Werror=type-limits on the IDF build).
    char* end = nullptr;
    errno = 0;
    const long long v = strtoll(event_val, &end, 10);
    if (end == event_val || *end != '\0' || errno == ERANGE ||
        v > INT32_MAX || v < INT32_MIN) {
        ESP_LOGW(TAG, "value expression: non-numeric trigger value '%s' — action skipped",
                 event_val);
        return false;
    }
    int32_t res = 0;
    if (!expr_eval(a.expr, (int32_t)v, res)) {
        ESP_LOGW(TAG, "value expression failed (division by zero?) — action skipped");
        return false;
    }
    snprintf(out, out_size, "%ld", (long)res);
    return true;
}

// ── Value comparison ──────────────────────────────────────────────────────

// Compare two pre-parsed int values using a conditional operator.
static bool compare_int(CondOp op, int32_t event_val, int32_t trig_val) {
    if (op == CondOp::ANY) return true;
    switch (op) {
    case CondOp::EQ:  return event_val == trig_val;
    case CondOp::NEQ: return event_val != trig_val;
    case CondOp::GT:  return event_val >  trig_val;
    case CondOp::LT:  return event_val <  trig_val;
    case CondOp::GTE: return event_val >= trig_val;
    case CondOp::LTE: return event_val <= trig_val;
    default:          return false;
    }
}

// ── Matcher ───────────────────────────────────────────────────────────────

bool simple_rules_match(const ParsedRule& rule, const Event& ev,
                        char* event_val, size_t val_size) {
    event_val[0] = '\0';
    const RuleTrigger& t = rule.trigger;

    switch (ev.type) {
    case EventType::ZCL_ATTR: {
        if (t.type != TriggerType::DEVICE_ATTR) return false;
        const auto& ze = *reinterpret_cast<const ZclAttrEvent*>(ev.data);
        // CODEX H-03: a DEVICE_ATTR trigger fires ONLY on its resolved device.
        // ieee == 0 means the friendly name never resolved (typo, renamed, not
        // yet paired). That is INERT, not a wildcard — the old `!= 0 &&`
        // short-circuit let an unresolved rule match events from every device,
        // so a typo could actuate an unrelated light/lock/script.
        if (t.ieee == 0 || t.ieee != ze.ieee) return false;
        // Empty attr_key = wildcard: match every attribute change on this
        // device. Useful for `ON friendly_name DO script.run "..."` where
        // the Lua handler inspects the incoming event table and decides
        // what to do. DSL parser leaves attr_key empty when the user
        // writes `ON <device>` with no `#<attr>` suffix.
        const bool wildcard_attr = (t.attr_key[0] == '\0');
        if (!wildcard_attr &&
            strncmp(t.attr_key, ze.key, ATTR_KEY_MAX) != 0) return false;

        // Fill event_val with a human-readable representation.
        if (ze.val_type == VAL_STR) {
            strncpy(event_val, ze.str_val, val_size - 1);
            event_val[val_size - 1] = '\0';
        } else {
            snprintf(event_val, val_size, "%d", (int)ze.int_val);
        }

        if (t.op == CondOp::ANY || wildcard_attr) return true;

        // VAL_FLOAT stores value × 100 as an int (the DSL literal is the ×100
        // value too, e.g. `temperature>2500` = 25.00); VAL_BOOL stores 0/1 in
        // int_val (the shadow bridge normalises the bool). Both share the
        // integer comparison domain as VAL_INT, and DSL binary/numeric literals
        // parse as VAL_INT — so fold both, letting `#occupancy=1` / `=0` match a
        // bool attr. VAL_STR is unchanged (an int literal can't match a string).
        const uint8_t attr_vt =
            (ze.val_type == VAL_FLOAT || ze.val_type == VAL_BOOL)
                ? (uint8_t)VAL_INT
                : ze.val_type;
        if (t.match_val_type != attr_vt) return false;

        if (t.match_val_type == VAL_STR) {
            // EQ/NEQ only — other ops don't make sense for string values.
            const bool eq = (strncmp(ze.str_val, t.str_val, ATTR_STR_MAX) == 0);
            if (t.op == CondOp::EQ)  return eq;
            if (t.op == CondOp::NEQ) return !eq;
            return false;
        }
        // INT/BOOL: both sides are integers.
        return compare_int(t.op, ze.int_val, t.int_val);
    }
    case EventType::CTRL_BOOT:
        return t.type == TriggerType::BOOT;

    case EventType::RULE_EVENT: {
        if (t.type != TriggerType::EVENT) return false;
        const auto& re = *reinterpret_cast<const RuleEventPayload*>(ev.data);
        return strcmp(t.key, re.name) == 0;
    }
    case EventType::RULE_TIMER_FIRE: {
        if (t.type != TriggerType::TIMER) return false;
        const auto& rtp = *reinterpret_cast<const RuleTimerPayload*>(ev.data);
        return atoi(t.key) == (int)rtp.timer_index;
    }
    case EventType::MQTT_MSG: {
        if (t.type != TriggerType::MQTT_TOPIC) return false;
        const auto& me = *reinterpret_cast<const MqttMsgEvent*>(ev.data);
        if (strcmp(t.key, me.topic) != 0) return false;
        strncpy(event_val, me.payload, val_size - 1);
        event_val[val_size - 1] = '\0';
        return true;
    }
    default:
        return false;
    }
}

// ── Edge triggers ─────────────────────────────────────────────────────────
// A device-attribute trigger fires on a change, not on every report: a
// comparison when it goes from not holding to holding, a bare `#attr` when the
// value differs from the last one. Sensors re-send their state as a heartbeat
// (a Tuya contact sensor every ~4 h), and the level-triggered engine re-ran the
// actions each time: the owner's socket switched itself on while nobody was
// home. The memory (RuleEdge) is seeded from the device shadow whenever a rule
// is (re)loaded, so a reboot, an edit or a rename never fires a rule either.

// Momentary attributes are events, not state: every report counts.
static bool is_momentary(const char* key) {
    static const char* const kMomentary[] = {"action", "click", "event", "scene"};
    for (const char* k : kMomentary)
        if (strcmp(key, k) == 0) return true;
    return false;
}

// Triggers that fire on a change and so keep memory. Not the bare
// `ON <device>` wildcard (it hands every report to a script) nor momentary keys.
static bool has_edge(const RuleTrigger& t) {
    return t.type == TriggerType::DEVICE_ATTR && t.ieee != 0 &&
           t.attr_key[0] != '\0' && !is_momentary(t.attr_key);
}

static bool same_value(const RuleEdge& e, const ZclAttrEvent& ze) {
    if ((e.val_type == VAL_STR) != (ze.val_type == VAL_STR)) return false;
    return ze.val_type == VAL_STR ? strncmp(e.str_val, ze.str_val, ATTR_STR_MAX) == 0
                                  : e.int_val == ze.int_val;
}

enum class Verdict : uint8_t { NONE, FIRE, UNCHANGED, CONDITION_FALSE };

// simple_rules_match plus the change test. NONE = the event is not about this
// rule's trigger. Updates the rule's trigger memory.
static Verdict evaluate(ParsedRule& r, const Event& ev, char* val, size_t val_size) {
    const bool holds = simple_rules_match(r, ev, val, val_size);
    const RuleTrigger& t = r.trigger;
    if (ev.type != EventType::ZCL_ATTR || t.type != TriggerType::DEVICE_ATTR)
        return holds ? Verdict::FIRE : Verdict::NONE;
    const auto& ze = *reinterpret_cast<const ZclAttrEvent*>(ev.data);
    if (t.ieee == 0 || t.ieee != ze.ieee ||
        (t.attr_key[0] != '\0' && strncmp(t.attr_key, ze.key, ATTR_KEY_MAX) != 0))
        return Verdict::NONE;
    if (!has_edge(t)) return holds ? Verdict::FIRE : Verdict::CONDITION_FALSE;

    RuleEdge& e = r.edge;
    const bool was = e.known && e.matched;
    const bool changed = !e.known || !same_value(e, ze);
    e.known    = true;
    e.matched  = holds;
    e.val_type = ze.val_type;
    if (ze.val_type == VAL_STR) memcpy(e.str_val, ze.str_val, ATTR_STR_MAX);
    else                        e.int_val = ze.int_val;
    if (!holds) return Verdict::CONDITION_FALSE;
    // Unknown counts as not matched: the first report that matches fires.
    return (t.op == CondOp::ANY ? changed : !was) ? Verdict::FIRE : Verdict::UNCHANGED;
}

// The device's current value of `key` from the shadow, as the ZCL_ATTR event a
// report of it would have made. False when the shadow has none.
static bool shadow_event(uint64_t ieee, const char* key, Event* ev) {
    ShadowAttr a{};
    if (!device_shadow_get_attr(ieee, key, &a)) return false;
    *ev = Event{};
    ev->type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev->data);
    ze.ieee     = ieee;
    ze.val_type = a.val_type;
    memcpy(ze.key, a.key, ATTR_KEY_MAX);
    if (a.val_type == VAL_STR) memcpy(ze.str_val, a.str_val, ATTR_STR_MAX);
    else                       ze.int_val = a.int_val;
    return true;
}

static bool same_trigger(const RuleTrigger& a, const RuleTrigger& b) {
    return a.type == b.type && a.ieee == b.ieee && a.op == b.op &&
           a.match_val_type == b.match_val_type && a.int_val == b.int_val &&
           strcmp(a.attr_key, b.attr_key) == 0 && strcmp(a.str_val, b.str_val) == 0;
}

// (Re)load a rule's trigger memory: the shadow's current value when it has one,
// else what `prev` (the rule this one replaces) knew for the same trigger, else
// unknown. The shadow drops keys past 32 per device while their reports still
// reach the rules, hence the fallback. `prev` may be `r` itself.
static void seed(ParsedRule& r, const ParsedRule* prev) {
    RuleEdge keep{};
    if (prev && same_trigger(prev->trigger, r.trigger)) keep = prev->edge;
    r.edge = RuleEdge{};
    if (!has_edge(r.trigger)) return;
    Event ev;
    char val[32];
    if (shadow_event(r.trigger.ieee, r.trigger.attr_key, &ev)) (void)evaluate(r, ev, val, sizeof(val));
    else r.edge = keep;
}

// ── Executor ─────────────────────────────────────────────────────────────

static const char* verb(ActionType t) {
    switch (t) {
    case ActionType::ZIGBEE_SET:    return "zigbee.set";
    case ActionType::ZIGBEE_TOGGLE: return "zigbee.toggle";
    case ActionType::PUBLISH:       return "publish";
    case ActionType::EVENT:         return "event";
    case ActionType::TIMER:         return "timer";
    case ActionType::LOG:           return "log";
    case ActionType::SCRIPT:        return "script.run";
    }
    return "?";
}

// Runs the actions in order. Returns 0 when each did its job, else the 1-based
// index of the first that failed (it logged why, except a publish MQTT refused).
static uint8_t execute_rule(const ParsedRule& rule, const char* event_val,
                            const Event* ev) {
    uint8_t failed = 0;
    for (uint8_t i = 0; i < rule.action_count; i++) {
        const RuleAction& a = rule.actions[i];
        auto fail = [&] { if (!failed) failed = static_cast<uint8_t>(i + 1); };
        switch (a.type) {

        case ActionType::ZIGBEE_SET: {
            char val_buf[32];
            if (!resolve_action_value(a, a.arg2, event_val,
                                      val_buf, sizeof(val_buf))) {
                fail();
                break;   // expression skip — send nothing, shadow untouched
            }

            // F35 (FINDINGS.md): resolve under the pool lock and snapshot
            // the device — never hold a raw pool pointer across the blocking
            // zhac_adapter_send_uint (a concurrent swap-with-last pool_remove
            // would otherwise relocate the slot and we'd actuate the wrong
            // device).
            ZapDevice snap; bool dev_found = false;
            zigbee_pool_lock();
            if (a.arg0[0] == '0' && (a.arg0[1] == 'x' || a.arg0[1] == 'X')) {
                uint64_t ieee = (uint64_t)strtoull(a.arg0, nullptr, 16);
                if (ZapDevice* d = pool_find_by_ieee(ieee)) { snap = *d; dev_found = true; }
            } else {
                ZapDevice* pool = pool_all();
                uint16_t cnt = pool_count();
                for (uint16_t j = 0; j < cnt; j++) {
                    if (strcmp(pool[j].friendly_name, a.arg0) == 0) {
                        snap = pool[j]; dev_found = true;
                        break;
                    }
                }
            }
            zigbee_pool_unlock();
            if (!dev_found) {
                ESP_LOGW(TAG, "zigbee.set: device '%s' not found", a.arg0);
                fail();
                break;
            }
            // `21.5` is a decimal write (the converter scales it); anything
            // else is the integer it always was. device_cmd does the send and
            // the optimistic shadow mirror, the same as every other transport.
            const bool decimal = strchr(val_buf, '.') != nullptr;
            const DevCmdValue val = decimal ? device_cmd_float(strtod(val_buf, nullptr))
                                            : device_cmd_int((int32_t)strtol(val_buf, nullptr, 10));
            const DevCmdResult r = device_cmd_set_attr(snap.ieee_addr, 0, a.arg1, &val);
            if (r != DEVCMD_OK) {
                ESP_LOGW(TAG, "zigbee.set: %s for '%s' key='%s'",
                         device_cmd_result_str(r), a.arg0, a.arg1);
                fail();
            }
            break;
        }

        case ActionType::ZIGBEE_TOGGLE: {
            // F35 (FINDINGS.md): resolve + snapshot under the pool lock.
            ZapDevice snap; bool dev_found = false;
            zigbee_pool_lock();
            if (a.arg0[0] == '0' && (a.arg0[1] == 'x' || a.arg0[1] == 'X')) {
                uint64_t ieee = (uint64_t)strtoull(a.arg0, nullptr, 16);
                if (ZapDevice* d = pool_find_by_ieee(ieee)) { snap = *d; dev_found = true; }
            } else {
                ZapDevice* pool = pool_all();
                uint16_t cnt = pool_count();
                for (uint16_t j = 0; j < cnt; j++) {
                    if (strcmp(pool[j].friendly_name, a.arg0) == 0) {
                        snap = pool[j]; dev_found = true;
                        break;
                    }
                }
            }
            zigbee_pool_unlock();
            if (!dev_found) {
                ESP_LOGW(TAG, "zigbee.toggle: device '%s' not found", a.arg0);
                fail();
                break;
            }
            // P2-T18 def 7 (FINDINGS §7): read just the one attr by key
            // instead of copying the whole 32-slot ShadowAttr array (~2.7 KB)
            // onto this shared event-drain task stack.
            ShadowAttr cur{};
            int32_t cur_int = -1;
            bool found = false;
            bool is_bool = false;
            if (device_shadow_get_attr(snap.ieee_addr, a.arg1, &cur) &&
                (cur.val_type == VAL_BOOL || cur.val_type == VAL_INT)) {
                cur_int = cur.int_val;
                is_bool = (cur.val_type == VAL_BOOL);
                found = true;
            }
            if (!found) {
                ESP_LOGW(TAG, "zigbee.toggle: attr '%s' on '%s' missing or non-numeric — skip",
                         a.arg1, a.arg0);
                fail();
                break;
            }
            if (cur_int != 0 && cur_int != 1 && !is_bool) {
                ESP_LOGW(TAG, "zigbee.toggle: '%s' on '%s' = %ld (not binary) — skip",
                         a.arg1, a.arg0, (long)cur_int);
                fail();
                break;
            }
            // The inverse, through the one attribute-set path (which mirrors it
            // into the shadow; a toggle used to leave the shadow stale).
            const DevCmdValue val = is_bool ? device_cmd_bool(cur_int == 0)
                                            : device_cmd_int(cur_int ? 0 : 1);
            const DevCmdResult r = device_cmd_set_attr(snap.ieee_addr, 0, a.arg1, &val);
            if (r != DEVCMD_OK) {
                ESP_LOGW(TAG, "zigbee.toggle: %s for '%s' key='%s'",
                         device_cmd_result_str(r), a.arg0, a.arg1);
                fail();
            }
            break;
        }

        case ActionType::PUBLISH: {
            char payload_buf[64];
            if (!resolve_action_value(a, a.arg1, event_val,
                                      payload_buf, sizeof(payload_buf))) {
                fail();
                break;   // expression skip — publish nothing
            }
            // False = not sent (MQTT off or not connected). Not logged: a hub
            // without MQTT would log it on every fire; the rule status shows it.
            if (!mqtt_gw_publish(a.arg0, payload_buf, strlen(payload_buf), 0, false))
                fail();
            break;
        }

        case ActionType::EVENT: {
            // Hop TTL: if this rule was itself triggered by a RULE_EVENT,
            // carry the chain length forward; any other trigger (attr/
            // boot/timer/MQTT) starts a fresh chain at hop 0. Refusing to
            // republish past MAX_EVENT_HOPS cuts self-feeding loops that
            // would otherwise wedge the drain loop forever.
            uint8_t src_hop = 0;
            if (ev && ev->type == EventType::RULE_EVENT)
                src_hop = reinterpret_cast<const RuleEventPayload*>(ev->data)->hop;
            if (src_hop >= MAX_EVENT_HOPS) {
                ESP_LOGW(TAG, "event '%s': rule event loop cut at hop %u (rule %u)",
                         a.arg0, src_hop, rule.rule_id);
                fail();
                break;
            }
            Event out{};
            out.type = EventType::RULE_EVENT;
            auto& p = *reinterpret_cast<RuleEventPayload*>(out.data);
            strncpy(p.name, a.arg0, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = '\0';
            p.hop = static_cast<uint8_t>(src_hop + 1);
            event_bus_publish(out);
            break;
        }

        case ActionType::TIMER: {
            int idx = atoi(a.arg0); // 1–8
            if (idx < 1 || idx > 8) {
                ESP_LOGW(TAG, "timer: invalid index %d", idx);
                fail();
                break;
            }
            const uint32_t ms = (uint32_t)strtoul(a.arg1, nullptr, 10);
            TimerHandle_t& tmr = s_timers[idx - 1];
            // `timer <n> 0` stops timer n, as Tasmota's RuleTimer<n> 0 does:
            // the motion recipe cancels its off-timer when motion comes back.
            // It used to mean "fire in 1 ms". A timer that never ran has
            // nothing to stop.
            if (ms == 0) {
                if (tmr && xTimerStop(tmr, 0) != pdPASS) fail();
                break;
            }
            // P1-T8: block-time-0 timer commands return pdFAIL on a full
            // timer command queue — previously ignored, so the rule's
            // timer action was silently lost. Warn (once per boot, this
            // can fire per event under a storm) so it's visible.
            bool armed = true;
            if (tmr) {
                if (xTimerChangePeriod(tmr, pdMS_TO_TICKS(ms), 0) != pdPASS)
                    armed = false;
                if (xTimerReset(tmr, 0) != pdPASS)
                    armed = false;
            } else {
                tmr = xTimerCreate("rule_tmr", pdMS_TO_TICKS(ms),
                                   pdFALSE, (void*)(uintptr_t)(uint8_t)idx, timer_cb);
                if (!tmr || xTimerStart(tmr, 0) != pdPASS)
                    armed = false;
            }
            if (!armed) {
                fail();
                static bool s_timer_warned = false;
                if (!s_timer_warned) {
                    s_timer_warned = true;
                    ESP_LOGW(TAG, "timer %d: arm failed (command queue full "
                                  "or create failed) — action lost; further "
                                  "drops suppressed", idx);
                }
            }
            break;
        }

        case ActionType::LOG:
            ESP_LOGI(TAG, "%s", a.arg0);
            break;

        case ActionType::SCRIPT: {
            if (!s_script_hook) {
                ESP_LOGW(TAG, "script.run '%s' skipped — no hook registered",
                         a.arg0);
                fail();
                break;
            }
            SimpleRulesScriptEvent sev{};
            sev.key   = "";
            sev.value = event_val ? event_val : "";
            sev.str_val = "";
            if (ev && ev->type == EventType::ZCL_ATTR) {
                const auto& ze = *reinterpret_cast<const ZclAttrEvent*>(ev->data);
                sev.ieee     = ze.ieee;
                sev.cluster  = ze.cluster;
                sev.attr_id  = ze.attr_id;
                sev.val_type = ze.val_type;
                sev.int_val  = ze.int_val;
                sev.key      = ze.key;
                sev.str_val  = (ze.val_type == VAL_STR) ? ze.str_val : "";
            }
            s_script_hook(a.arg0, sev);
            break;
        }

        default:
            break;
        }
    }
    return failed;
}

// ── Firing: one log line, the actions, the run counters ─────────────────

// Seconds since boot. Not time(): a run before the clock is set must still
// read right once it is.
static uint32_t mono_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec);
}

static void log_fire(const ParsedRule& r, const char* val, const Event* ev, bool manual) {
    const RuleTrigger& t = r.trigger;
    char what[112] = "";
    switch (t.type) {
    case TriggerType::DEVICE_ATTR: {
        char dev[sizeof(t.device_name) + 4];
        if (t.device_name[0]) snprintf(dev, sizeof(dev), "%s", t.device_name);
        else snprintf(dev, sizeof(dev), "0x%016" PRIX64, t.ieee);
        const char* key = t.attr_key;   // the wildcard names the reported one
        if (!key[0] && ev && ev->type == EventType::ZCL_ATTR)
            key = reinterpret_cast<const ZclAttrEvent*>(ev->data)->key;
        snprintf(what, sizeof(what), "%s#%.*s=%s", dev, (int)ATTR_KEY_MAX, key, val);
        break;
    }
    case TriggerType::TIME_CRON:  snprintf(what, sizeof(what), "Time#Cron"); break;
    case TriggerType::EVENT:      snprintf(what, sizeof(what), "Event#%s", t.key); break;
    case TriggerType::TIMER:      snprintf(what, sizeof(what), "Rules#Timer=%s", t.key); break;
    case TriggerType::MQTT_TOPIC: snprintf(what, sizeof(what), "Mqtt#%s=%s", t.key, val); break;
    case TriggerType::BOOT:       snprintf(what, sizeof(what), "System#Boot"); break;
    }
    const char* how = manual ? "run now" : "fired";
    char who[sizeof(r.name) + 8];
    if (r.name[0]) snprintf(who, sizeof(who), "'%s'", r.name);
    else           snprintf(who, sizeof(who), "#%u", (unsigned)r.rule_id);
    // A timer or cron rule may run every few seconds: DEBUG, so it does not
    // bury the log. Device, MQTT, event and manual runs stay at INFO.
    if (!manual && (t.type == TriggerType::TIMER || t.type == TriggerType::TIME_CRON))
        ESP_LOGD(TAG, "rule %s %s (%s)", who, how, what);
    else
        ESP_LOGI(TAG, "rule %s %s (%s)", who, how, what);
}

// Runs rule `id` (at `idx` when matched) from a copy taken under the lock, as
// the actions can block on MQTT / the radio / Lua, then books the run on the
// live rule. `snap` is the caller's scratch (the event and cron tasks have the
// stack for it). False when the rule is gone, or disabled and not `manual`.
static bool fire_rule(ParsedRule& snap, uint16_t idx, uint16_t id,
                      const char* val, const Event* ev, bool manual) {
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    // Re-validate the slot in case it was edited or deleted since the match.
    const bool live = idx < s_rule_count && s_rules[idx].rule_id == id &&
                      (manual || s_rules[idx].enabled);
    if (live) snap = s_rules[idx];
    xSemaphoreGiveRecursive(s_mutex);
    if (!live) return false;

    log_fire(snap, val, ev, manual);
    const uint8_t failed = execute_rule(snap, val, ev);

    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (idx < s_rule_count && s_rules[idx].rule_id == id) {
            RuleStat& st = s_rules[idx].stat;
            st.runs++;
            st.fired_mono  = mono_s();
            st.skip        = failed ? RuleSkip::ACTION_ERROR : RuleSkip::NONE;
            st.skip_action = failed;
        }
        xSemaphoreGiveRecursive(s_mutex);
    }
    return true;
}

// ── Event dispatch ────────────────────────────────────────────────────────

static void dispatch_event(const Event& ev) {
    // Event-loop protection lives in the EVENT action (MAX_EVENT_HOPS TTL
    // carried in RuleEventPayload), not here — see the comment at the
    // constant for why a dispatch-depth counter could never trip.

    // LUA-F8 + CC-F5: do not hold s_mutex across action dispatch.
    // Snapshot the matching rule indices + their stringified event
    // values under a bounded-timeout lock, then drop the lock before
    // running the actions (which can block on MQTT/SPI/Lua). fire_rule
    // copies each matching rule on a retake so an in-flight edit
    // invalidates rather than tears. The trigger memory is updated here,
    // under the lock, in event order.
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "dispatch_event: s_mutex contended >2s — event dropped");
        return;
    }
    static constexpr uint8_t MAX_MATCHED_PER_EVENT = 16;
    uint16_t matched_idx[MAX_MATCHED_PER_EVENT];
    char     matched_val[MAX_MATCHED_PER_EVENT][32];
    uint16_t matched_id [MAX_MATCHED_PER_EVENT];
    uint8_t  matched_count = 0;
    for (uint16_t i = 0; i < s_rule_count && matched_count < MAX_MATCHED_PER_EVENT; i++) {
        if (!s_rules[i].enabled) continue;
        char event_val[32] = {};
        switch (evaluate(s_rules[i], ev, event_val, sizeof(event_val))) {
        case Verdict::FIRE:
            matched_idx[matched_count] = i;
            matched_id [matched_count] = s_rules[i].rule_id;
            std::snprintf(matched_val[matched_count], sizeof(matched_val[0]),
                          "%s", event_val);
            matched_count++;
            break;
        case Verdict::UNCHANGED:       s_rules[i].stat.skip = RuleSkip::UNCHANGED;       break;
        case Verdict::CONDITION_FALSE: s_rules[i].stat.skip = RuleSkip::CONDITION_FALSE; break;
        case Verdict::NONE:            break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);

    ParsedRule snap;
    for (uint8_t i = 0; i < matched_count; i++)
        fire_rule(snap, matched_idx[i], matched_id[i], matched_val[i], &ev, false);
}

// ── Cron task ─────────────────────────────────────────────────────────────

// No ZHAC board has an RTC (see zap_clock.h). A schedule matched against the
// 1970 power-on clock fires at the wrong time -- "0 7 * * *" seven hours after
// boot -- so cron rules and Lua cron handlers wait until the clock is set.

static void task_cron(void*) {
    // The parser now accepts an optional 6th field (seconds), so the
    // firing loop ticks once per second instead of once per minute.
    // Legacy 5-field rules still fire only at second :00 of the
    // matched minute because cron_parse sets second_bits = bit 0 for
    // that form. The match check is a handful of bit-ANDs per rule.
    time_t last_evaluated = 0;  // dedupe so wall-clock skew can't fire a rule twice in one second
    bool   waiting_for_clock = false;

    for (;;) {
        time_t now = time(nullptr);
        if (!zap_clock_is_set(now)) {
            if (!waiting_for_clock) {
                ESP_LOGW(TAG, "cron: clock not set yet -- scheduled rules wait for it");
                waiting_for_clock = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (waiting_for_clock) {
            ESP_LOGI(TAG, "cron: clock set -- scheduled rules running");
            waiting_for_clock = false;
        }
        if (now == last_evaluated) {
            // Sub-second drift landed us in the same wall-clock second
            // we already evaluated. Sleep a fraction and retry rather
            // than firing twice.
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        last_evaluated = now;

        // Same snapshot-then-exec pattern as dispatch_event (LUA-F8):
        // collect the firing cron rules under a timeout-bounded lock,
        // then drop the lock before action dispatch.
        if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "task_cron: s_mutex contended >2s — tick skipped");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        static constexpr uint8_t MAX_CRON_FIRES = 16;
        uint16_t fire_idx[MAX_CRON_FIRES];
        uint16_t fire_id [MAX_CRON_FIRES];
        uint8_t  fire_count = 0;
        for (uint16_t i = 0; i < s_rule_count && fire_count < MAX_CRON_FIRES; i++) {
            if (!s_rules[i].enabled) continue;
            if (s_rules[i].trigger.type != TriggerType::TIME_CRON) continue;
            // F27: parse once and cache (invalidated on rule-list changes).
            if (s_cron_state[i] == CRON_UNCACHED) {
                s_cron_state[i] = cron_parse(s_rules[i].trigger.key, s_cron_cache[i])
                                      ? CRON_GOOD : CRON_BAD;
            }
            if (s_cron_state[i] != CRON_GOOD) continue;
            if (cron_matches(s_cron_cache[i], now)) {
                fire_idx[fire_count] = i;
                fire_id [fire_count] = s_rules[i].rule_id;
                fire_count++;
            }
        }
        xSemaphoreGiveRecursive(s_mutex);

        ParsedRule snap;
        for (uint8_t i = 0; i < fire_count; i++)
            fire_rule(snap, fire_idx[i], fire_id[i], "", nullptr, false);
        // Sleep ~1 s, but resync to the next wall-clock second so a
        // drifted RTC adjustment doesn't shift the firing offset.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// CODEX H-03: a device joined or left — re-resolve friendly-name triggers so a
// rule authored before the device paired binds to it (and a removed device's
// stale binding is dropped) deterministically, not only on the next reload.
// A rule bound to another device now (or to one for the first time, like on the
// P4 where rules load before the pool and shadow are restored) starts from that
// device's shadow; a rule still on the same device keeps its memory.
static void on_pool_change(const Event&) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < s_rule_count; i++) {
        const uint64_t before = s_rules[i].trigger.ieee;
        simple_rules_resolve_names(&s_rules[i], 1);
        if (s_rules[i].trigger.ieee != before) seed(s_rules[i], nullptr);
    }
    xSemaphoreGiveRecursive(s_mutex);
}

// ── Public API ────────────────────────────────────────────────────────────

void simple_rules_init() {
    s_rules = static_cast<ParsedRule*>(
        heap_caps_calloc(MAX_CACHED_RULES, sizeof(ParsedRule), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(s_rules);
    s_mutex = xSemaphoreCreateRecursiveMutex();

    event_bus_subscribe(EventType::ZCL_ATTR,       dispatch_event);
    event_bus_subscribe(EventType::CTRL_BOOT,       dispatch_event);
    event_bus_subscribe(EventType::RULE_EVENT,      dispatch_event);
    event_bus_subscribe(EventType::RULE_TIMER_FIRE, dispatch_event);
    event_bus_subscribe(EventType::MQTT_MSG,        dispatch_event);
    // CODEX H-03: re-resolve friendly-name triggers when the device pool changes.
    event_bus_subscribe(EventType::DEVICE_JOIN,     on_pool_change);
    event_bus_subscribe(EventType::DEVICE_LEAVE,    on_pool_change);

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    reload_locked();
    xSemaphoreGiveRecursive(s_mutex);

    zhac_task_create(task_cron, "rule_cron", zhac::stack::kRuleCron, nullptr, 2, nullptr);
}

static void publish_changed(uint16_t rule_id, uint8_t change) {
    Event ev{};
    ev.type = EventType::RULE_CHANGED;
    const RuleChangedEvent p{rule_id, change};
    memcpy(ev.data, &p, sizeof(p));
    event_bus_publish(ev);
}

void simple_rules_reload() {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    reload_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

bool simple_rules_add(const char* name, const char* dsl,
                       uint16_t* out_rule_id) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    // P2-T18 def 4 (FINDINGS §7): a DSL that doesn't fit slot.src would parse
    // in full for the live rule yet persist truncated, so the rule silently
    // mutates / fails to parse after reboot. Reject up-front — no
    // truncate-persist path. (REST/cloud contract is ≤499 B anyway.)
    size_t dsl_len = dsl ? strlen(dsl) : 0;
    if (dsl_len >= sizeof(((RuleSlot*)nullptr)->src)) {
        dsl_set_last_error("rule DSL too long (max 499 bytes)");
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    // P2-T18 def 2 (FINDINGS §7): the active-rule cache is full. The old code
    // still persisted to NVS and returned true, so the rule was accepted but
    // never evaluated — a silent no-op. Fail explicitly instead; the message
    // reaches the SPA/cloud via the HAP RULE_EXEC_RESULT err field.
    if (s_rule_count >= MAX_CACHED_RULES) {
        dsl_set_last_error("rule cache full (max 64 active rules)");
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    uint16_t id = next_rule_id();
    ParsedRule r{};
    if (dsl_parse(dsl, id, &r) != ParseResult::OK) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    RuleSlot slot{};
    slot.rule_id      = id;
    slot.enabled      = 1;
    slot._reserved    = 0;
    slot.rule_type    = (uint8_t)RuleType::SIMPLE;
    slot.trigger_type = (uint8_t)r.trigger.type;
    if (name) {
        strncpy(slot.name, name, sizeof(slot.name) - 1);
        slot.name[sizeof(slot.name) - 1] = '\0';
    }
    slot.src_len      = (uint16_t)dsl_len;   // validated < sizeof(slot.src) above
    memcpy(slot.src, dsl, dsl_len);
    // Deferred NVS commit — in-memory s_rules is authoritative at runtime;
    // PSRAM writeback task flushes to flash ≤5 s later. Saves ~10–50 ms
    // per REST round-trip and cuts flash wear on rapid edits.
    rule_store_mark_dirty(&slot);
    r.enabled = true;
    memcpy(r.name, slot.name, sizeof(r.name));
    ParsedRule& added = s_rules[s_rule_count++];
    added = r;
    simple_rules_resolve_names(&added, 1);
    seed(added, nullptr);      // the door already open: its heartbeat must not fire
    cron_cache_invalidate();   // F27
    if (out_rule_id) *out_rule_id = id;
    xSemaphoreGiveRecursive(s_mutex);
    publish_changed(id, RULE_CHANGE_ADDED);
    return true;
}

bool simple_rules_update(uint16_t rule_id,
                          const char* name, const char* dsl) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    // P2-T18 def 4 (FINDINGS §7): reject oversize DSL up-front — no
    // truncate-persist (see simple_rules_add).
    size_t dsl_len = dsl ? strlen(dsl) : 0;
    if (dsl_len >= sizeof(((RuleSlot*)nullptr)->src)) {
        dsl_set_last_error("rule DSL too long (max 499 bytes)");
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    ParsedRule r{};
    if (dsl_parse(dsl, rule_id, &r) != ParseResult::OK) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    // Resolve the rule's current enabled state and confirm it EXISTS before
    // persisting. update() must not (a) resurrect a disabled rule by forcing
    // enabled=1, nor (b) create an orphan when rule_id is unknown. A rule can
    // live in the cache, or only in NVS (cache overflow at load — see the store
    // fallback in simple_rules_delete).
    int cache_idx = -1;
    for (uint16_t i = 0; i < s_rule_count; i++) {
        if (s_rules[i].rule_id == rule_id) { cache_idx = (int)i; break; }
    }
    uint8_t was_enabled;
    if (cache_idx >= 0) {
        was_enabled = s_rules[cache_idx].enabled ? 1 : 0;
    } else {
        RuleSlot existing{};
        if (!rule_store_load(rule_id, &existing)) {
            xSemaphoreGiveRecursive(s_mutex);
            return false;   // unknown rule_id — do not persist an orphan
        }
        was_enabled = existing.enabled ? 1 : 0;
    }

    RuleSlot slot{};
    slot.rule_id      = rule_id;
    slot.enabled      = was_enabled;   // preserve current state, don't force-enable
    slot._reserved    = 0;
    slot.rule_type    = (uint8_t)RuleType::SIMPLE;
    slot.trigger_type = (uint8_t)r.trigger.type;
    if (name) {
        strncpy(slot.name, name, sizeof(slot.name) - 1);
        slot.name[sizeof(slot.name) - 1] = '\0';
    }
    slot.src_len      = (uint16_t)dsl_len;   // validated < sizeof(slot.src) above
    memcpy(slot.src, dsl, dsl_len);
    rule_store_mark_dirty(&slot);  // deferred NVS commit (see simple_rules_add)
    if (cache_idx >= 0) {
        ParsedRule& cur = s_rules[cache_idx];
        r.enabled = was_enabled;
        r.stat = cur.stat;                     // an edit keeps the run counters
        memcpy(r.name, slot.name, sizeof(r.name));
        simple_rules_resolve_names(&r, 1);
        seed(r, &cur);                         // an edit never fires the rule
        cur = r;
        cron_cache_invalidate();   // F27
    }
    xSemaphoreGiveRecursive(s_mutex);
    publish_changed(rule_id, RULE_CHANGE_UPDATED);
    return true;
}

bool simple_rules_delete(uint16_t rule_id) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    bool found = false;
    for (uint16_t i = 0; i < s_rule_count; i++) {
        if (s_rules[i].rule_id == rule_id) {
            s_rules[i] = s_rules[--s_rule_count];
            cron_cache_invalidate();   // F27: slot→rule mapping changed
            found = true;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);
    // Also check the NVS-backed store — a rule may exist on disk but be
    // absent from the in-memory cache if MAX_CACHED_RULES was exceeded
    // at load time. Only emit the tombstone when either side had it.
    if (!found) {
        RuleSlot tmp{};
        if (!rule_store_load(rule_id, &tmp)) return false;
    }
    rule_store_mark_delete(rule_id);  // deferred NVS commit
    publish_changed(rule_id, RULE_CHANGE_DELETED);
    return true;
}

bool simple_rules_enable(uint16_t rule_id, bool enabled) {
    RuleSlot slot{};
    if (!rule_store_load(rule_id, &slot)) return false;
    slot.enabled = enabled ? 1 : 0;
    rule_store_mark_dirty(&slot);  // deferred NVS commit
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < s_rule_count; i++) {
        if (s_rules[i].rule_id == rule_id) {
            s_rules[i].enabled = enabled;
            // Re-resolve the device-name → IEEE binding here. The
            // initial resolution happens at reload_locked time, but if
            // the device pool changes afterwards (device re-paired, or
            // the pool wasn't populated yet at boot), t.ieee can stay
            // bound to a stale IEEE and the matcher silently drops
            // every event. The user-visible recovery was previously a
            // DSL re-save; making enable refresh bindings means a
            // disable/enable toggle alone is enough to recover.
            // A disabled rule saw no reports: start again from the shadow, so
            // the heartbeat after re-enabling does not fire it.
            if (enabled) {
                simple_rules_resolve_names(&s_rules[i], 1);
                seed(s_rules[i], &s_rules[i]);
            }
            break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);
    publish_changed(rule_id, RULE_CHANGE_UPDATED);
    return true;
}

uint16_t simple_rules_list(RuleSlot* out, uint16_t max_count) {
    return rule_store_load_all(out, max_count);
}

uint16_t simple_rules_status(SimpleRuleStatus* out, uint16_t max_count) {
    if (!out || !s_mutex) return 0;
    const uint32_t mono = mono_s();
    const time_t   now  = time(nullptr);
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    uint16_t n = 0;
    for (; n < s_rule_count && n < max_count; n++) {
        const ParsedRule& r = s_rules[n];
        SimpleRuleStatus& o = out[n];
        o = SimpleRuleStatus{};
        o.rule_id = r.rule_id;
        o.runs    = r.stat.runs;
        if (r.stat.runs) {
            o.ago_s      = mono - r.stat.fired_mono;
            o.last_fired = zap_clock_is_set(now) ? static_cast<uint32_t>(now - o.ago_s)
                                                 : r.stat.fired_mono;
        }
        switch (r.stat.skip) {
        case RuleSkip::UNCHANGED:       snprintf(o.last_skip, sizeof(o.last_skip), "unchanged");       break;
        case RuleSkip::CONDITION_FALSE: snprintf(o.last_skip, sizeof(o.last_skip), "condition_false"); break;
        case RuleSkip::ACTION_ERROR: {
            const uint8_t k = r.stat.skip_action;
            snprintf(o.last_skip, sizeof(o.last_skip), "action_error:%s",
                     k >= 1 && k <= r.action_count ? verb(r.actions[k - 1].type) : "?");
            break;
        }
        case RuleSkip::NONE: break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);
    return n;
}

bool simple_rules_run_now(uint16_t rule_id) {
    if (!s_mutex) return false;
    // Heap, not stack: this runs on a request task (WebSocket, cloud relay)
    // whose stack was not sized for a ~1 KB ParsedRule copy.
    auto* snap = static_cast<ParsedRule*>(heap_caps_malloc(sizeof(ParsedRule), MALLOC_CAP_SPIRAM));
    if (!snap) snap = static_cast<ParsedRule*>(malloc(sizeof(ParsedRule)));
    if (!snap) return false;
    // %value% and the script's event: the trigger attribute's current value.
    Event ev{};
    char val[32] = "";
    bool have_ev = false;
    int idx = -1;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < s_rule_count && idx < 0; i++)
        if (s_rules[i].rule_id == rule_id) idx = i;
    if (idx >= 0) {
        const RuleTrigger& t = s_rules[idx].trigger;
        have_ev = t.type == TriggerType::DEVICE_ATTR && t.ieee != 0 && t.attr_key[0] != '\0' &&
                  shadow_event(t.ieee, t.attr_key, &ev);
        if (have_ev) (void)simple_rules_match(s_rules[idx], ev, val, sizeof(val));   // fills val
    }
    xSemaphoreGiveRecursive(s_mutex);
    const bool ok = idx >= 0 &&
                    fire_rule(*snap, static_cast<uint16_t>(idx), rule_id, val, have_ev ? &ev : nullptr, true);
    free(snap);
    return ok;
}

void simple_rules_resolve_names(ParsedRule* rules, uint16_t count) {
    // F35 (FINDINGS.md): read the pool under the advisory lock — this scans
    // friendly_name/ieee_addr while another task may swap-with-last remove.
    // Pure reads, short scan, no blocking calls, so holding the lock is safe.
    zigbee_pool_lock();
    ZapDevice* pool = pool_all();
    uint16_t   cnt  = pool_count();
    for (uint16_t i = 0; i < count; i++) {
        RuleTrigger& t = rules[i].trigger;
        if (t.type != TriggerType::DEVICE_ATTR) continue;
        if (t.device_name[0] == '\0') continue;   // ieee literal — nothing to resolve
        // CODEX H-03: re-resolve by name on EVERY pass — do not skip an already
        // non-zero ieee. A device that was removed or re-paired with a new
        // address otherwise keeps a stale binding forever (the old
        // `if (t.ieee != 0) continue`). A name that no longer matches resolves
        // back to 0, which the matcher treats as inert (never a wildcard).
        uint64_t resolved = 0;
        for (uint16_t j = 0; j < cnt; j++) {
            if (strcmp(pool[j].friendly_name, t.device_name) == 0) {
                resolved = pool[j].ieee_addr;
                break;
            }
        }
        if (resolved == 0) {
            ESP_LOGW(TAG, "rule %u: device \"%s\" not paired — trigger inert until it joins",
                     (unsigned)rules[i].rule_id, t.device_name);
        }
        t.ieee = resolved;
    }
    zigbee_pool_unlock();
}
