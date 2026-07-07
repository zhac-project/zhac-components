// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Comprehensive coverage of the simple_rules ACTION side (execute_rule):
//   • expand_value %value% substitution (event value → the emitted set value);
//   • zigbee.set value/key/type routing (state→VAL_BOOL, other→VAL_INT);
//   • script.run → the registered hook, incl. the SimpleRulesScriptEvent
//     trigger context and quoted names;
//   • event emit → chaining into a second rule, and the MAX_EVENT_HOPS
//     self-feed cut (a `ON Event#x DO … ; event x` loop must terminate);
//   • multiple `;`-separated actions in one rule;
//   • action parse rejections (missing arg, unknown verb, empty DO, no ENDON,
//     >4 actions);
//   • device-not-found and toggle-without-cached-value no-ops.
//
// The adapter/mqtt sinks discard their args in the host stubs, so effects are
// observed through the optimistic-shadow recorder (stub_shadow_opt_*), the
// script hook, and the real event bus. Companion to test_action_shadow (which
// pins the optimistic-write-on-send-success behaviour specifically).
#include "simple_rules.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

extern void        stub_pool_seed(const ZapDevice* dev);
extern void        stub_shadow_opt_reset(void);
extern int         stub_shadow_opt_count(void);
extern const char* stub_shadow_opt_key(void);
extern uint8_t     stub_shadow_opt_vt(void);
extern int32_t     stub_shadow_opt_val(void);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kIeee = 0xAABBCCDDEEFF0001ULL;

// ── Script-hook capture ────────────────────────────────────────────────────
static struct {
    int      calls;
    char     name[64];
    char     value[32];
    uint64_t ieee;
    uint8_t  vt;
    int32_t  iv;
} g_script;

static void on_script(const char* name, const SimpleRulesScriptEvent& ev) {
    g_script.calls++;
    std::strncpy(g_script.name, name ? name : "", sizeof(g_script.name) - 1);
    g_script.name[sizeof(g_script.name) - 1] = '\0';
    std::strncpy(g_script.value, ev.value ? ev.value : "", sizeof(g_script.value) - 1);
    g_script.value[sizeof(g_script.value) - 1] = '\0';
    g_script.ieee = ev.ieee;
    g_script.vt   = ev.val_type;
    g_script.iv   = ev.int_val;
}
static void script_reset() { std::memset(&g_script, 0, sizeof(g_script)); }

// ── Event helpers ──────────────────────────────────────────────────────────
static void publish_attr(const char* key, uint8_t vt, int32_t v) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = kIeee;
    ze.val_type = vt;
    std::strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    ze.key[ATTR_KEY_MAX - 1] = '\0';
    ze.int_val = v;
    event_bus_publish(ev);
}
static void publish_rule_event(const char* name, uint8_t hop) {
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto& re = *reinterpret_cast<RuleEventPayload*>(ev.data);
    std::strncpy(re.name, name, sizeof(re.name) - 1);
    re.name[sizeof(re.name) - 1] = '\0';
    re.hop = hop;
    event_bus_publish(ev);
}
static void drain_attr()   { for (int i = 0; i < 8;  i++) if (event_bus_drain(EventType::ZCL_ATTR, 0) == 0)   break; }
static void drain_events() { for (int i = 0; i < 40; i++) if (event_bus_drain(EventType::RULE_EVENT, 0) == 0) break; }

static uint16_t add(const char* name, const char* dsl) {
    uint16_t id = 0;
    if (!simple_rules_add(name, dsl, &id)) {
        printf("  (add failed for '%s': %s)\n", dsl, dsl_last_error());
        return 0;
    }
    return id;
}

int main() {
    event_bus_init();
    simple_rules_init();
    simple_rules_set_script_hook(on_script);

    ZapDevice dev{};
    dev.ieee_addr      = kIeee;
    dev.nwk_addr       = 0x1274;
    dev.endpoints[0]   = 1;
    dev.endpoint_count = 1;
    std::strncpy(dev.friendly_name, "lamp", sizeof(dev.friendly_name) - 1);
    std::strncpy(dev.model_id, "TS0503B", sizeof(dev.model_id) - 1);
    stub_pool_seed(&dev);

    // ── 1. expand_value %value% + literal value routing ──────────────────
    {
        stub_shadow_opt_reset();
        uint16_t id = add("a1", "ON s1#lux=200 DO zigbee.set lamp brightness %value% ENDON");
        publish_attr("lux", VAL_INT, 200); drain_attr();
        CHECK(stub_shadow_opt_count() == 1 &&
              strcmp(stub_shadow_opt_key(), "brightness") == 0 &&
              stub_shadow_opt_vt() == VAL_INT && stub_shadow_opt_val() == 200,
              "%value% substitutes the event value (brightness=200)");
        simple_rules_delete(id);

        stub_shadow_opt_reset();
        id = add("a1b", "ON s1#lux=5 DO zigbee.set lamp brightness 77 ENDON");
        publish_attr("lux", VAL_INT, 5); drain_attr();
        CHECK(stub_shadow_opt_count() == 1 && stub_shadow_opt_val() == 77,
              "literal value passes through verbatim (brightness=77)");
        simple_rules_delete(id);

        stub_shadow_opt_reset();
        id = add("a1c", "ON s1#occupancy=1 DO zigbee.set lamp state %value% ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 1 &&
              strcmp(stub_shadow_opt_key(), "state") == 0 &&
              stub_shadow_opt_vt() == VAL_BOOL && stub_shadow_opt_val() == 1,
              "state key routes to VAL_BOOL, %value%→1");
        simple_rules_delete(id);
    }

    // ── 2. script.run → hook + trigger context ───────────────────────────
    {
        script_reset();
        uint16_t id = add("a2", "ON s2#occupancy=1 DO script.run myscript ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(g_script.calls == 1 && strcmp(g_script.name, "myscript") == 0,
              "script.run fires the hook with the script name");
        CHECK(strcmp(g_script.value, "1") == 0 && g_script.ieee == kIeee &&
              g_script.vt == VAL_BOOL && g_script.iv == 1,
              "hook receives the trigger context (value/ieee/type)");
        simple_rules_delete(id);

        script_reset();
        id = add("a2b", "ON s2#occupancy=1 DO script.run \"my script\" ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(g_script.calls == 1 && strcmp(g_script.name, "my script") == 0,
              "quoted script name preserved");
        simple_rules_delete(id);

        // A non-matching event must NOT invoke the hook.
        script_reset();
        id = add("a2c", "ON s2#occupancy=1 DO script.run nope ENDON");
        publish_attr("occupancy", VAL_BOOL, 0); drain_attr();
        CHECK(g_script.calls == 0, "hook not called when the condition fails");
        simple_rules_delete(id);
    }

    // ── 3. event emit → chained rule fires ───────────────────────────────
    {
        stub_shadow_opt_reset();
        uint16_t a = add("a3a", "ON s3#occupancy=1 DO event dusk ENDON");
        uint16_t b = add("a3b", "ON Event#dusk DO zigbee.set lamp state 1 ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr(); drain_events();
        CHECK(stub_shadow_opt_count() == 1 &&
              strcmp(stub_shadow_opt_key(), "state") == 0 && stub_shadow_opt_val() == 1,
              "event emit chains into a second rule's action");
        simple_rules_delete(a); simple_rules_delete(b);
    }

    // ── 4. MAX_EVENT_HOPS self-feed cut (must terminate, bounded) ────────
    {
        stub_shadow_opt_reset();
        uint16_t id = add("a4", "ON Event#loop DO zigbee.set lamp state 1 ; event loop ENDON");
        publish_rule_event("loop", 0);
        drain_events();
        // hop 0..8 fire the set (9×); hop 8 is the last that does NOT re-emit
        // (src_hop >= MAX_EVENT_HOPS=8). Without the cut this would not
        // terminate; the count proves it stopped at the TTL.
        const int n = stub_shadow_opt_count();
        printf("  (self-feed fired %d times)\n", n);
        CHECK(n == 9, "self-feeding event loop cut at MAX_EVENT_HOPS (9 fires)");
        simple_rules_delete(id);
    }

    // ── 5. multiple ';'-separated actions ────────────────────────────────
    {
        stub_shadow_opt_reset();
        uint16_t id = add("a5", "ON s5#occupancy=1 DO zigbee.set lamp state 1 ; zigbee.set lamp brightness 50 ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 2 &&
              strcmp(stub_shadow_opt_key(), "brightness") == 0 && stub_shadow_opt_val() == 50,
              "both ';'-separated actions execute (2 writes, last=brightness 50)");
        simple_rules_delete(id);
    }

    // ── 6. action parse rejections ───────────────────────────────────────
    {
        uint16_t id = 0;
        CHECK(!simple_rules_add("e1", "ON s6#occupancy=1 DO zigbee.toggle lamp ENDON", &id),
              "zigbee.toggle without an attr is rejected");
        CHECK(!simple_rules_add("e2", "ON s6#occupancy=1 DO frobnicate x ENDON", &id),
              "unknown action verb is rejected");
        CHECK(!simple_rules_add("e3", "ON s6#occupancy=1 DO ENDON", &id),
              "empty DO block is rejected");
        CHECK(!simple_rules_add("e4", "ON s6#occupancy=1 DO log hi", &id),
              "missing ENDON is rejected");
        CHECK(!simple_rules_add("e5",
              "ON s6#occupancy=1 DO log a ; log b ; log c ; log d ; log e ENDON", &id),
              "more than 4 actions is rejected");
    }

    // ── 7. device-not-found → no send, no shadow write ───────────────────
    {
        stub_shadow_opt_reset();
        uint16_t id = add("a7", "ON s7#occupancy=1 DO zigbee.set ghost state 1 ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 0, "zigbee.set on an unknown device is a no-op");
        simple_rules_delete(id);
    }

    // ── 8. zigbee.toggle with no cached value → skip ─────────────────────
    {
        stub_shadow_opt_reset();
        uint16_t id = add("a8", "ON s8#occupancy=1 DO zigbee.toggle lamp state ENDON");
        publish_attr("occupancy", VAL_BOOL, 1); drain_attr();
        CHECK(stub_shadow_opt_count() == 0, "toggle with no cached shadow value is skipped");
        simple_rules_delete(id);
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
