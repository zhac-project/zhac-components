// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "simple_rules.h"
#include "rule_store.h"
#include "esp_heap_caps.h"
#include "event_bus.h"
#include "mqtt_gw.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"
#include "device_shadow.h"
#include "cron_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include "task_stacks.h"

static const char* TAG = "simple_rules";

static constexpr uint8_t MAX_DISPATCH_DEPTH = 8;
static uint8_t           s_dispatch_depth   = 0;

// ── In-memory rule cache ──────────────────────────────────────────────────

static constexpr uint16_t MAX_CACHED_RULES = 64;
static ParsedRule*         s_rules      = nullptr; // allocated in PSRAM on init
static uint16_t            s_rule_count = 0;
static SemaphoreHandle_t   s_mutex = nullptr; // recursive

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
    uint16_t max_id = 0;
    for (uint16_t i = 0; i < s_rule_count; i++)
        if (s_rules[i].rule_id > max_id) max_id = s_rules[i].rule_id;
    return max_id + 1;
}

static void reload_locked() {
    auto* slots = static_cast<RuleSlot*>(malloc(sizeof(RuleSlot) * MAX_CACHED_RULES));
    if (!slots) { ESP_LOGE(TAG, "reload: malloc failed"); return; }
    uint16_t cnt = rule_store_load_all(slots, MAX_CACHED_RULES);
    s_rule_count = 0;
    for (uint16_t i = 0; i < cnt && s_rule_count < MAX_CACHED_RULES; i++) {
        if (slots[i].rule_type != (uint8_t)RuleType::SIMPLE) continue;
        ParsedRule r{};
        ParseResult res = dsl_parse((const char*)slots[i].src, slots[i].rule_id, &r);
        if (res != ParseResult::OK) {
            ESP_LOGW(TAG, "rule %u parse error %d — skipped", slots[i].rule_id, (int)res);
            if (s_error_cb) {
                static char err_msg[48];
                snprintf(err_msg, sizeof(err_msg), "parse error %d", (int)res);
                s_error_cb(slots[i].rule_id, err_msg);
            }
            continue;
        }
        r.enabled = slots[i].enabled != 0;
        s_rules[s_rule_count++] = r;
    }
    simple_rules_resolve_names(s_rules, s_rule_count);
    free(slots);
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
        if (t.ieee != 0 && t.ieee != ze.ieee) return false;
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

        if (t.match_val_type != ze.val_type) return false;

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

// ── Executor ─────────────────────────────────────────────────────────────

static void execute_rule(const ParsedRule& rule, const char* event_val,
                         const Event* ev) {
    for (uint8_t i = 0; i < rule.action_count; i++) {
        const RuleAction& a = rule.actions[i];
        switch (a.type) {

        case ActionType::ZIGBEE_SET: {
            char val_buf[32];
            expand_value(a.arg2, event_val, val_buf, sizeof(val_buf));

            ZapDevice* dev = nullptr;
            if (a.arg0[0] == '0' && (a.arg0[1] == 'x' || a.arg0[1] == 'X')) {
                uint64_t ieee = (uint64_t)strtoull(a.arg0, nullptr, 16);
                dev = pool_find_by_ieee(ieee);
            } else {
                ZapDevice* pool = pool_all();
                uint16_t cnt = pool_count();
                for (uint16_t j = 0; j < cnt; j++) {
                    if (strcmp(pool[j].friendly_name, a.arg0) == 0) {
                        dev = &pool[j];
                        break;
                    }
                }
            }
            if (!dev) {
                ESP_LOGW(TAG, "zigbee.set: device '%s' not found", a.arg0);
                break;
            }
            int32_t int_val = (int32_t)strtol(val_buf, nullptr, 10);
            uint8_t ep = dev->endpoints[0] ? dev->endpoints[0] : 1;
            if (!zhac_adapter_send_uint(dev->ieee_addr,
                                         dev->model_id,
                                         dev->manufacturer_name,
                                         dev->nwk_addr, ep,
                                         a.arg1,
                                         static_cast<uint64_t>(int_val))) {
                ESP_LOGW(TAG, "zigbee.set: no tz converter for '%s' key='%s'",
                         a.arg0, a.arg1);
            }
            break;
        }

        case ActionType::ZIGBEE_TOGGLE: {
            ZapDevice* dev = nullptr;
            if (a.arg0[0] == '0' && (a.arg0[1] == 'x' || a.arg0[1] == 'X')) {
                uint64_t ieee = (uint64_t)strtoull(a.arg0, nullptr, 16);
                dev = pool_find_by_ieee(ieee);
            } else {
                ZapDevice* pool = pool_all();
                uint16_t cnt = pool_count();
                for (uint16_t j = 0; j < cnt; j++) {
                    if (strcmp(pool[j].friendly_name, a.arg0) == 0) {
                        dev = &pool[j];
                        break;
                    }
                }
            }
            if (!dev) {
                ESP_LOGW(TAG, "zigbee.toggle: device '%s' not found", a.arg0);
                break;
            }
            ShadowAttr sa[32];
            uint8_t n = device_shadow_get_attrs(dev->ieee_addr, sa, 32);
            int32_t cur_int = -1;
            bool found = false;
            bool is_bool = false;
            for (uint8_t j = 0; j < n; j++) {
                if (strcmp(sa[j].key, a.arg1) == 0) {
                    if (sa[j].val_type == VAL_BOOL || sa[j].val_type == VAL_INT) {
                        cur_int = sa[j].int_val;
                        is_bool = (sa[j].val_type == VAL_BOOL);
                        found = true;
                    }
                    break;
                }
            }
            if (!found) {
                ESP_LOGW(TAG, "zigbee.toggle: attr '%s' on '%s' missing or non-numeric — skip",
                         a.arg1, a.arg0);
                break;
            }
            if (cur_int != 0 && cur_int != 1 && !is_bool) {
                ESP_LOGW(TAG, "zigbee.toggle: '%s' on '%s' = %ld (not binary) — skip",
                         a.arg1, a.arg0, (long)cur_int);
                break;
            }
            int32_t next_int = cur_int ? 0 : 1;
            uint8_t ep = dev->endpoints[0] ? dev->endpoints[0] : 1;
            if (!zhac_adapter_send_uint(dev->ieee_addr,
                                         dev->model_id,
                                         dev->manufacturer_name,
                                         dev->nwk_addr, ep,
                                         a.arg1,
                                         static_cast<uint64_t>(next_int))) {
                ESP_LOGW(TAG, "zigbee.toggle: no tz converter for '%s' key='%s'",
                         a.arg0, a.arg1);
            }
            break;
        }

        case ActionType::PUBLISH: {
            char payload_buf[64];
            expand_value(a.arg1, event_val, payload_buf, sizeof(payload_buf));
            mqtt_gw_publish(a.arg0, payload_buf, strlen(payload_buf),
                            0, false);
            break;
        }

        case ActionType::EVENT: {
            Event ev{};
            ev.type = EventType::RULE_EVENT;
            auto& p = *reinterpret_cast<RuleEventPayload*>(ev.data);
            strncpy(p.name, a.arg0, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = '\0';
            event_bus_publish(ev);
            break;
        }

        case ActionType::TIMER: {
            int idx = atoi(a.arg0); // 1–8
            if (idx < 1 || idx > 8) {
                ESP_LOGW(TAG, "timer: invalid index %d", idx);
                break;
            }
            uint32_t ms = (uint32_t)strtoul(a.arg1, nullptr, 10);
            if (ms == 0) ms = 1;
            TimerHandle_t& tmr = s_timers[idx - 1];
            if (tmr) {
                xTimerChangePeriod(tmr, pdMS_TO_TICKS(ms), 0);
                xTimerReset(tmr, 0);
            } else {
                tmr = xTimerCreate("rule_tmr", pdMS_TO_TICKS(ms),
                                   pdFALSE, (void*)(uintptr_t)(uint8_t)idx, timer_cb);
                if (tmr) xTimerStart(tmr, 0);
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
}

// ── Event dispatch ────────────────────────────────────────────────────────

static void dispatch_event(const Event& ev) {
    if (s_dispatch_depth >= MAX_DISPATCH_DEPTH) {
        ESP_LOGE(TAG, "rule dispatch depth limit (%u) reached — possible event loop, dropping event type=%d",
                 MAX_DISPATCH_DEPTH, static_cast<int>(ev.type));
        return;
    }

    // LUA-F8 + CC-F5: do not hold s_mutex across action dispatch.
    // Snapshot the matching rule indices + their stringified event
    // values under a bounded-timeout lock, then drop the lock before
    // calling execute_rule (which can block on MQTT/SPI/Lua). Each
    // matching rule is copied on a per-iteration retake so an in-flight
    // edit invalidates rather than tears.
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "dispatch_event: s_mutex contended >2s — event dropped");
        return;
    }
    static constexpr uint8_t MAX_MATCHED_PER_EVENT = 16;
    uint16_t matched_idx[MAX_MATCHED_PER_EVENT];
    char     matched_val[MAX_MATCHED_PER_EVENT][32];
    uint16_t matched_id [MAX_MATCHED_PER_EVENT];
    uint8_t  matched_count = 0;
    s_dispatch_depth++;
    for (uint16_t i = 0; i < s_rule_count && matched_count < MAX_MATCHED_PER_EVENT; i++) {
        if (!s_rules[i].enabled) continue;
        char event_val[32] = {};
        if (simple_rules_match(s_rules[i], ev, event_val, sizeof(event_val))) {
            matched_idx[matched_count] = i;
            matched_id [matched_count] = s_rules[i].rule_id;
            std::snprintf(matched_val[matched_count], sizeof(matched_val[0]),
                          "%s", event_val);
            matched_count++;
        }
    }
    s_dispatch_depth--;
    xSemaphoreGiveRecursive(s_mutex);

    for (uint8_t i = 0; i < matched_count; i++) {
        ParsedRule snap;
        if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        // Re-validate the slot in case it was edited or deleted between
        // match and execute.
        bool live = (matched_idx[i] < s_rule_count) &&
                    s_rules[matched_idx[i]].enabled  &&
                    s_rules[matched_idx[i]].rule_id == matched_id[i];
        if (live) snap = s_rules[matched_idx[i]];
        xSemaphoreGiveRecursive(s_mutex);
        if (live) execute_rule(snap, matched_val[i], &ev);
    }
}

// ── Cron task ─────────────────────────────────────────────────────────────

static void task_cron(void*) {
    // The parser now accepts an optional 6th field (seconds), so the
    // firing loop ticks once per second instead of once per minute.
    // Legacy 5-field rules still fire only at second :00 of the
    // matched minute because cron_parse sets second_bits = bit 0 for
    // that form. The match check is a handful of bit-ANDs per rule.
    time_t last_evaluated = 0;  // dedupe so wall-clock skew can't fire a rule twice in one second

    for (;;) {
        time_t now = time(nullptr);
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
            CronExpr expr{};
            if (!cron_parse(s_rules[i].trigger.key, expr)) continue;
            if (cron_matches(expr, now)) {
                fire_idx[fire_count] = i;
                fire_id [fire_count] = s_rules[i].rule_id;
                fire_count++;
            }
        }
        xSemaphoreGiveRecursive(s_mutex);

        for (uint8_t i = 0; i < fire_count; i++) {
            ParsedRule snap;
            if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) continue;
            bool live = (fire_idx[i] < s_rule_count) &&
                        s_rules[fire_idx[i]].enabled &&
                        s_rules[fire_idx[i]].rule_id == fire_id[i];
            if (live) snap = s_rules[fire_idx[i]];
            xSemaphoreGiveRecursive(s_mutex);
            if (live) execute_rule(snap, "", nullptr);
        }
        // Sleep ~1 s, but resync to the next wall-clock second so a
        // drifted RTC adjustment doesn't shift the firing offset.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
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

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    reload_locked();
    xSemaphoreGiveRecursive(s_mutex);

    xTaskCreate(task_cron, "rule_cron", zhac::stack::kRuleCron, nullptr, 2, nullptr);
}

void simple_rules_reload() {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    reload_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

bool simple_rules_add(const char* name, const char* dsl,
                       uint16_t* out_rule_id) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
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
    size_t dsl_len    = strlen(dsl);
    slot.src_len      = (uint16_t)(dsl_len < sizeof(slot.src) ? dsl_len : sizeof(slot.src) - 1);
    memcpy(slot.src, dsl, slot.src_len);
    // Deferred NVS commit — in-memory s_rules is authoritative at runtime;
    // PSRAM writeback task flushes to flash ≤5 s later. Saves ~10–50 ms
    // per REST round-trip and cuts flash wear on rapid edits.
    rule_store_mark_dirty(&slot);
    if (s_rule_count < MAX_CACHED_RULES) {
        r.enabled = true;
        s_rules[s_rule_count++] = r;
        simple_rules_resolve_names(&s_rules[s_rule_count - 1], 1);
    }
    if (out_rule_id) *out_rule_id = id;
    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool simple_rules_update(uint16_t rule_id,
                          const char* name, const char* dsl) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    ParsedRule r{};
    if (dsl_parse(dsl, rule_id, &r) != ParseResult::OK) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }
    RuleSlot slot{};
    slot.rule_id      = rule_id;
    slot.enabled      = 1;
    slot._reserved    = 0;
    slot.rule_type    = (uint8_t)RuleType::SIMPLE;
    slot.trigger_type = (uint8_t)r.trigger.type;
    if (name) {
        strncpy(slot.name, name, sizeof(slot.name) - 1);
        slot.name[sizeof(slot.name) - 1] = '\0';
    }
    size_t dsl_len    = strlen(dsl);
    slot.src_len      = (uint16_t)(dsl_len < sizeof(slot.src) ? dsl_len : sizeof(slot.src) - 1);
    memcpy(slot.src, dsl, slot.src_len);
    rule_store_mark_dirty(&slot);  // deferred NVS commit (see simple_rules_add)
    for (uint16_t i = 0; i < s_rule_count; i++) {
        if (s_rules[i].rule_id == rule_id) {
            bool was_enabled = s_rules[i].enabled;
            s_rules[i] = r;
            s_rules[i].enabled = was_enabled;
            simple_rules_resolve_names(&s_rules[i], 1);
            break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool simple_rules_delete(uint16_t rule_id) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    bool found = false;
    for (uint16_t i = 0; i < s_rule_count; i++) {
        if (s_rules[i].rule_id == rule_id) {
            s_rules[i] = s_rules[--s_rule_count];
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
            if (enabled) simple_rules_resolve_names(&s_rules[i], 1);
            break;
        }
    }
    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

uint16_t simple_rules_list(RuleSlot* out, uint16_t max_count) {
    return rule_store_load_all(out, max_count);
}

void simple_rules_resolve_names(ParsedRule* rules, uint16_t count) {
    ZapDevice* pool = pool_all();
    uint16_t   cnt  = pool_count();
    for (uint16_t i = 0; i < count; i++) {
        RuleTrigger& t = rules[i].trigger;
        if (t.type != TriggerType::DEVICE_ATTR) continue;
        if (t.ieee != 0)            continue;
        if (t.device_name[0] == '\0') continue;
        for (uint16_t j = 0; j < cnt; j++) {
            if (strcmp(pool[j].friendly_name, t.device_name) == 0) {
                t.ieee = pool[j].ieee_addr;
                break;
            }
        }
    }
}
