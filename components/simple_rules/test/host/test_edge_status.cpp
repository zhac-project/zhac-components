// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests: edge-triggered device rules, rule status and "Run now".
//
// A device-attribute trigger fires on a change: a comparison (`#contact=1`,
// `#temperature>2500`) when it goes from not holding to holding, a bare
// `#attr` when the value differs from the last one. The trigger memory is
// seeded from the device shadow whenever rules are (re)loaded (boot, save,
// edit, the reload after a rename), so neither a sensor heartbeat nor a
// reboot re-runs a rule. The owner's Tuya contact sensor re-sends contact=1
// every ~4 h and switched a socket on by itself each time. Momentary
// attributes (action, click, event, scene) and every non-device trigger keep
// firing on each event. Status (runs, last run, last skip reason) and Run now
// live in RAM beside the parsed rule, outside the rule objects the cloud diffs.
//
// Every rule runs `script.run <tag>`; the script hook is the fire counter.
// report() mirrors the real pipeline: device_shadow caches the value first,
// then publishes the ZCL_ATTR event the rule engine sees.
#include "simple_rules.h"
#include "rule_store.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <ctime>

extern void stub_pool_seed(const ZapDevice* dev);
extern void stub_shadow_set(uint64_t ieee, const char* key, uint8_t vt, int32_t iv, const char* sv);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kDoor     = 0xA100000000000001ULL;
static constexpr uint64_t kDoor2    = 0xA100000000000002ULL;
static constexpr uint64_t kBootDoor = 0xA100000000000003ULL;
static constexpr uint64_t kThermo   = 0xA100000000000004ULL;
static constexpr uint64_t kCube     = 0xA100000000000005ULL;
static constexpr uint64_t kLamp     = 0xA100000000000006ULL;

// ── Fire counter ───────────────────────────────────────────────────────────
struct Hit { char name[32]; int count; char value[32]; char key[ATTR_KEY_MAX]; uint64_t ieee; };
static Hit g_hits[32];
static int g_nhits = 0;

static void on_script(const char* name, const SimpleRulesScriptEvent& ev) {
    Hit* h = nullptr;
    for (int i = 0; i < g_nhits; i++) if (strcmp(g_hits[i].name, name) == 0) h = &g_hits[i];
    if (!h) {
        if (g_nhits == 32) return;
        h = &g_hits[g_nhits++];
        snprintf(h->name, sizeof h->name, "%s", name);
    }
    h->count++;
    snprintf(h->value, sizeof h->value, "%s", ev.value ? ev.value : "");
    snprintf(h->key, sizeof h->key, "%s", ev.key ? ev.key : "");
    h->ieee = ev.ieee;
}
static const Hit* hit(const char* name) {
    for (int i = 0; i < g_nhits; i++) if (strcmp(g_hits[i].name, name) == 0) return &g_hits[i];
    return nullptr;
}
static int fires(const char* name) { const Hit* h = hit(name); return h ? h->count : 0; }

// ── Event helpers ──────────────────────────────────────────────────────────
static void drain(EventType t) { for (int i = 0; i < 8; i++) if (event_bus_drain(t, 0) == 0) break; }

static void publish_attr(uint64_t ieee, const char* key, uint8_t vt, int32_t v, const char* s) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = ieee;
    ze.val_type = vt;
    std::strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    if (vt == VAL_STR) std::strncpy(ze.str_val, s, ATTR_STR_MAX - 1);
    else ze.int_val = v;
    event_bus_publish(ev);
    drain(EventType::ZCL_ATTR);
}
// A device report as the hub handles it: shadow first, then the event.
static void report(uint64_t ieee, const char* key, uint8_t vt, int32_t v) {
    stub_shadow_set(ieee, key, vt, v, nullptr);
    publish_attr(ieee, key, vt, v, nullptr);
}
static void report_str(uint64_t ieee, const char* key, const char* s) {
    stub_shadow_set(ieee, key, VAL_STR, 0, s);
    publish_attr(ieee, key, VAL_STR, 0, s);
}
// A report the shadow could not keep (its 32-attribute cap): event only.
static void report_uncached(uint64_t ieee, const char* key, uint8_t vt, int32_t v) {
    publish_attr(ieee, key, vt, v, nullptr);
}

static void seed_device(uint64_t ieee, const char* name) {
    ZapDevice d{};
    d.ieee_addr = ieee;
    d.nwk_addr = 0x1000;
    d.endpoints[0] = 1;
    d.endpoint_count = 1;
    std::strncpy(d.friendly_name, name, sizeof(d.friendly_name) - 1);
    stub_pool_seed(&d);
}

static uint16_t add(const char* name, const char* dsl) {
    uint16_t id = 0;
    if (!simple_rules_add(name, dsl, &id)) printf("  (add failed for '%s': %s)\n", dsl, dsl_last_error());
    return id;
}

// A rule saved before the reboot: straight into the (stub) store.
static void persist(uint16_t id, const char* name, const char* dsl) {
    RuleSlot s{};
    s.rule_id = id;
    s.enabled = 1;
    s.rule_type = (uint8_t)RuleType::SIMPLE;
    std::strncpy(s.name, name, sizeof(s.name) - 1);
    s.src_len = (uint16_t)strlen(dsl);
    memcpy(s.src, dsl, s.src_len);
    rule_store_mark_dirty(&s);
}

int main() {
    event_bus_init();
    simple_rules_set_script_hook(on_script);
    seed_device(kDoor, "door");
    seed_device(kDoor2, "door2");
    seed_device(kBootDoor, "bootdoor");
    seed_device(kThermo, "thermo");
    seed_device(kCube, "cube");
    seed_device(kLamp, "lamp");

    // ── 1. Boot: rules saved before the reboot, shadow restored from flash ──
    persist(1, "boot_contact", "ON bootdoor#contact=1 DO script.run boot_contact ENDON");
    persist(2, "boot_tamper", "ON bootdoor#tamper=1 DO script.run boot_tamper ENDON");
    stub_shadow_set(kBootDoor, "contact", VAL_BOOL, 1, nullptr);   // the door was open
    simple_rules_init();
    report(kBootDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("boot_contact") == 0,
          "boot: seeded from the restored shadow, the first equal report does not fire");
    report(kBootDoor, "tamper", VAL_BOOL, 1);
    CHECK(fires("boot_tamper") == 1,
          "boot: an attribute with no known value is unknown, the first match fires");
    report(kBootDoor, "contact", VAL_BOOL, 0);
    report(kBootDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("boot_contact") == 1, "boot: a real close then open after the reboot fires");

    // ── 2. The heartbeat (the owner's bug) ────────────────────────────────
    const uint16_t hb = add("heartbeat", "ON door#contact=1 DO script.run hb ENDON");
    report(kDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1, "unknown -> matched fires");
    report(kDoor, "contact", VAL_BOOL, 1);
    report(kDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1, "a heartbeat with the same value does not fire again");

    // ── 3. Bare #attr fires when the value changes ────────────────────────
    add("follow", "ON door2#contact DO script.run follow ENDON");   // the owner's rule shape
    report(kDoor2, "contact", VAL_BOOL, 1);
    CHECK(fires("follow") == 1 && strcmp(hit("follow")->value, "1") == 0,
          "bare #attr: the first report fires (unknown value)");
    report(kDoor2, "contact", VAL_BOOL, 1);
    CHECK(fires("follow") == 1, "bare #attr: the same value again does not fire");
    report(kDoor2, "contact", VAL_BOOL, 0);
    CHECK(fires("follow") == 2 && strcmp(hit("follow")->value, "0") == 0,
          "bare #attr: a changed value fires, %value% is the new value");
    report(kDoor2, "contact", VAL_BOOL, 0);
    report(kDoor2, "contact", VAL_BOOL, 1);
    CHECK(fires("follow") == 3, "bare #attr: 0, 0, 1 fires once more (on the change)");

    // ── 4. Comparison transitions ─────────────────────────────────────────
    const uint16_t hot = add("hot", "ON thermo#temperature>2500 DO script.run hot ENDON");
    report(kThermo, "temperature", VAL_FLOAT, 2400);
    CHECK(fires("hot") == 0, "comparison: false does not fire");
    report(kThermo, "temperature", VAL_FLOAT, 2600);
    CHECK(fires("hot") == 1, "comparison: false -> true fires");
    report(kThermo, "temperature", VAL_FLOAT, 2700);
    CHECK(fires("hot") == 1, "comparison: true -> true does not fire");
    report(kThermo, "temperature", VAL_FLOAT, 2400);
    report(kThermo, "temperature", VAL_FLOAT, 2600);
    CHECK(fires("hot") == 2, "comparison: true -> false -> true fires again");

    // ── 5. Momentary attributes keep firing on every report ───────────────
    add("shake", "ON cube#action=\"shake\" DO script.run shake ENDON");
    report_str(kCube, "action", "shake");
    report_str(kCube, "action", "shake");
    report_str(kCube, "action", "shake");
    CHECK(fires("shake") == 3, "momentary: action=\"shake\" fires on every shake");
    report_str(kCube, "action", "rotate_left");
    CHECK(fires("shake") == 3, "momentary: another action does not fire it");
    add("m_action", "ON cube#action DO script.run m_action ENDON");
    add("m_click", "ON cube#click DO script.run m_click ENDON");
    add("m_event", "ON cube#event DO script.run m_event ENDON");
    add("m_scene", "ON cube#scene=3 DO script.run m_scene ENDON");
    report_str(kCube, "action", "rotate_left");
    report_str(kCube, "action", "rotate_left");
    report_str(kCube, "click", "single");
    report_str(kCube, "click", "single");
    report_str(kCube, "event", "wake");
    report_str(kCube, "event", "wake");
    report(kCube, "scene", VAL_INT, 3);
    report(kCube, "scene", VAL_INT, 3);
    CHECK(fires("m_action") == 2 && fires("m_click") == 2 && fires("m_event") == 2 &&
          fires("m_scene") == 2,
          "momentary: action / click / event / scene fire on each repeated report");

    // ── 6. A reload, a rename, an edit never fire a rule ──────────────────
    // The shadow holds door contact=1 and door2 contact=1 (report() cached them).
    const uint16_t other = add("other", "ON Event#noop DO log x ENDON");
    simple_rules_update(other, "other", "ON Event#noop DO log y ENDON");
    report(kDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1, "an unrelated rule edit does not re-fire a rule");
    simple_rules_update(hb, "heartbeat", "ON door#contact=1 DO script.run hb ; log edited ENDON");
    report(kDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1, "editing the rule reseeds it from the shadow: no fire");
    // What device_cmd_rename's hook does: the device takes a new name, rules reload.
    std::strncpy(pool_find_by_ieee(kLamp)->friendly_name, "desk_lamp", sizeof(ZapDevice::friendly_name) - 1);
    simple_rules_reload();
    report(kDoor, "contact", VAL_BOOL, 1);
    report(kDoor2, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1 && fires("follow") == 3,
          "the reload after a device rename does not re-fire (comparison and bare #attr)");
    report(kDoor2, "contact", VAL_BOOL, 0);
    CHECK(fires("follow") == 4, "after the reload a real change still fires");
    // An attribute the shadow could not keep: the rule's own memory survives.
    add("humid", "ON thermo#humidity>7000 DO script.run humid ENDON");
    report_uncached(kThermo, "humidity", VAL_INT, 7500);
    CHECK(fires("humid") == 1, "uncached attribute: unknown -> matched fires");
    simple_rules_reload();
    report_uncached(kThermo, "humidity", VAL_INT, 7500);
    CHECK(fires("humid") == 1, "uncached attribute: the rule remembers it across a reload");
    // Disable, the door closes and opens, enable: the heartbeat must not fire.
    simple_rules_enable(hb, false);
    report(kDoor, "contact", VAL_BOOL, 0);
    report(kDoor, "contact", VAL_BOOL, 1);
    simple_rules_enable(hb, true);
    report(kDoor, "contact", VAL_BOOL, 1);
    CHECK(fires("hb") == 1, "re-enabling reseeds from the shadow: a heartbeat does not fire");

    // ── 7. Non-device triggers keep today's behaviour (every event) ──────
    add("ev", "ON Event#go DO script.run ev ENDON");
    add("tm", "ON Rules#Timer=2 DO script.run tm ENDON");
    add("mq", "ON Mqtt#zhac/x DO script.run mq ENDON");
    add("bt", "ON System#Boot DO script.run bt ENDON");
    add("wild", "ON cube DO script.run wild ENDON");
    for (int i = 0; i < 2; i++) {
        Event e{};
        e.type = EventType::RULE_EVENT;
        std::strncpy(reinterpret_cast<RuleEventPayload*>(e.data)->name, "go", 94);
        event_bus_publish(e);
        drain(EventType::RULE_EVENT);

        Event t{};
        t.type = EventType::RULE_TIMER_FIRE;
        reinterpret_cast<RuleTimerPayload*>(t.data)->timer_index = 2;
        event_bus_publish(t);
        drain(EventType::RULE_TIMER_FIRE);

        Event m{};
        m.type = EventType::MQTT_MSG;
        std::strncpy(reinterpret_cast<MqttMsgEvent*>(m.data)->topic, "zhac/x", 63);
        std::strncpy(reinterpret_cast<MqttMsgEvent*>(m.data)->payload, "on", 31);
        event_bus_publish(m);
        drain(EventType::MQTT_MSG);

        Event b{};
        b.type = EventType::CTRL_BOOT;
        event_bus_publish(b);
        drain(EventType::CTRL_BOOT);

        report(kCube, "battery", VAL_INT, 90);
    }
    CHECK(fires("ev") == 2 && fires("tm") == 2 && fires("mq") == 2 && fires("bt") == 2,
          "Event# / Rules#Timer / Mqtt# / System#Boot fire on every event, as before");
    CHECK(fires("wild") == 2, "the bare `ON <device>` wildcard fires on every report, as before");

    // ── 8. Status: runs, last run, last skip reason ──────────────────────
    static SimpleRuleStatus st[64];
    auto status_of = [](uint16_t id) -> const SimpleRuleStatus* {
        const uint16_t n = simple_rules_status(st, 64);
        for (uint16_t i = 0; i < n; i++) if (st[i].rule_id == id) return &st[i];
        return nullptr;
    };
    const uint32_t now = (uint32_t)time(nullptr);
    const SimpleRuleStatus* s = status_of(hb);
    CHECK(s && s->runs == 1, "status: runs counts the fires since boot");
    CHECK(s && s->last_fired + 5 >= now && s->last_fired <= now && s->ago_s <= 5,
          "status: last_fired is the wall-clock time of the last run (clock set), ago_s is small");
    CHECK(s && strcmp(s->last_skip, "unchanged") == 0,
          "status: a report that changed nothing is recorded as 'unchanged'");
    report(kThermo, "temperature", VAL_FLOAT, 2400);
    s = status_of(hot);
    CHECK(s && s->runs == 2 && strcmp(s->last_skip, "condition_false") == 0,
          "status: a report that fails the comparison is recorded as 'condition_false'");
    report(kThermo, "temperature", VAL_FLOAT, 2600);
    s = status_of(hot);
    CHECK(s && s->runs == 3 && s->last_skip[0] == '\0', "status: a clean run clears the skip reason");
    const uint16_t boom = add("boom", "ON cube#action=\"boom\" DO log boom ; zigbee.set ghost state 1 ENDON");
    report_str(kCube, "action", "boom");
    s = status_of(boom);
    CHECK(s && s->runs == 1 && strcmp(s->last_skip, "action_error:zigbee.set") == 0,
          "status: a failing action is recorded as 'action_error:<verb>'");
    const uint16_t idle = add("idle", "ON Event#never DO log x ENDON");
    s = status_of(idle);
    CHECK(s && s->runs == 0 && s->last_fired == 0 && s->last_skip[0] == '\0',
          "status: a rule that never ran reports runs 0, last_fired 0");
    s = status_of(1);
    CHECK(s && s->runs == 1, "status: counters survive the reloads above (boot rule, 1 run)");

    // ── 9. Run now ─────────────────────────────────────────────────────────
    const uint16_t rn = add("rn", "ON thermo#pressure>1100 DO script.run rn ENDON");
    stub_shadow_set(kThermo, "pressure", VAL_INT, 1013, nullptr);   // below the threshold
    CHECK(simple_rules_run_now(rn) && fires("rn") == 1,
          "Run now runs the actions although the comparison does not hold");
    CHECK(fires("rn") == 1 && strcmp(hit("rn")->value, "1013") == 0,
          "Run now: %value% is the trigger attribute's current shadow value");
    CHECK(fires("rn") == 1 && hit("rn")->ieee == kThermo && strcmp(hit("rn")->key, "pressure") == 0,
          "Run now: the script sees the trigger device and attribute");
    s = status_of(rn);
    CHECK(s && s->runs == 1 && s->last_skip[0] == '\0', "Run now counts as a run");
    report(kThermo, "pressure", VAL_INT, 1150);
    CHECK(fires("rn") == 2, "Run now leaves the trigger memory alone: the next real crossing fires");
    const uint16_t rn2 = add("rn2", "ON Event#x DO script.run rn2 ENDON");
    CHECK(simple_rules_run_now(rn2) && fires("rn2") == 1 && hit("rn2")->value[0] == '\0',
          "Run now of a non-device rule: %value% is empty");
    simple_rules_enable(rn2, false);
    CHECK(simple_rules_run_now(rn2) && fires("rn2") == 2, "Run now also runs a disabled rule");
    CHECK(!simple_rules_run_now(9999), "Run now of an unknown rule id fails");

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
