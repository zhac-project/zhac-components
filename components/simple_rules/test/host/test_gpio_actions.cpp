// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// gpio.set / gpio.toggle / pwm.set (spec extra/docs/superpowers/specs/2026-11-15-gpio-control-design.md §3):
// the grammar, the firmware's pin check at save time, and what reaches the run hook at fire time.
#include "simple_rules.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

extern void stub_pool_seed(const ZapDevice* dev);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kIeee = 0xAABBCCDDEEFF0042ULL;

static struct { int calls; SimpleRulesIoOp op; char pin[32]; char value[32]; bool result; } g_run;
static bool on_run(SimpleRulesIoOp op, const char* pin, const char* value) {
    g_run.calls++;
    g_run.op = op;
    std::snprintf(g_run.pin, sizeof(g_run.pin), "%s", pin);
    std::snprintf(g_run.value, sizeof(g_run.value), "%s", value);
    return g_run.result;
}
static void run_reset() { std::memset(&g_run, 0, sizeof(g_run)); g_run.result = true; }

// The firmware's check: outputs "relay" (also GPIO 21), pwm "fan". g_refuse_all = the owner removed every pin.
static bool g_refuse_all = false;
static bool on_check(SimpleRulesIoOp op, const char* pin, char* err, size_t cap) {
    if (g_refuse_all) { std::snprintf(err, cap, "unknown pin '%s' (outputs: none configured)", pin); return false; }
    const bool ok = op == SimpleRulesIoOp::PWM ? std::strcmp(pin, "fan") == 0
                                               : (std::strcmp(pin, "relay") == 0 || std::strcmp(pin, "21") == 0);
    if (!ok) std::snprintf(err, cap, "unknown pin '%s' (%s: %s)", pin,
                           op == SimpleRulesIoOp::PWM ? "pwm" : "outputs", op == SimpleRulesIoOp::PWM ? "fan" : "relay");
    return ok;
}

static void publish_attr(const char* key, int32_t v) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = kIeee;
    ze.val_type = VAL_INT;
    std::snprintf(ze.key, ATTR_KEY_MAX, "%s", key);
    ze.int_val = v;
    event_bus_publish(ev);
}
static void drain() { for (int i = 0; i < 8; i++) if (event_bus_drain(EventType::ZCL_ATTR, 0) == 0) break; }

static bool parses(const char* actions) {
    char dsl[200];
    std::snprintf(dsl, sizeof(dsl), "ON io#p DO %s ENDON", actions);
    char err[96];
    return simple_rules_check(dsl, err, sizeof(err));
}

static uint16_t add(const char* dsl) {
    uint16_t id = 0;
    if (!simple_rules_add("g", dsl, &id)) { printf("  (add failed for '%s': %s)\n", dsl, dsl_last_error()); return 0; }
    return id;
}

static bool last_skip_is(uint16_t id, const char* want) {
    SimpleRuleStatus st[16];
    const uint16_t n = simple_rules_status(st, 16);
    for (uint16_t i = 0; i < n; i++) if (st[i].rule_id == id) return std::strcmp(st[i].last_skip, want) == 0;
    return false;
}

int main() {
    event_bus_init();
    simple_rules_init();

    ZapDevice dev{};
    dev.ieee_addr = kIeee;
    std::snprintf(dev.friendly_name, sizeof(dev.friendly_name), "io");
    stub_pool_seed(&dev);

    // ── grammar (no check hook: any pin token) ──
    CHECK(parses("gpio.set relay on"), "gpio.set <name> on");
    CHECK(parses("gpio.set 21 off"), "gpio.set <number> off");
    CHECK(parses("gpio.set relay HIGH") && parses("gpio.set relay low") && parses("gpio.set relay True") &&
          parses("gpio.set relay 0") && parses("gpio.set relay 1"), "gpio.set accepts on/off/1/0/true/false/high/low, any case");
    CHECK(parses("gpio.set relay %value%") && parses("gpio.set relay %value%-1"), "gpio.set takes %value% and expressions");
    CHECK(parses("gpio.toggle relay"), "gpio.toggle <pin>");
    CHECK(parses("pwm.set fan 40") && parses("pwm.set fan 0") && parses("pwm.set fan 100") && parses("pwm.set fan 12.5"),
          "pwm.set 0..100, decimals allowed");
    CHECK(parses("pwm.set fan %value%/2"), "pwm.set takes an expression");
    CHECK(parses("gpio.set relay on; pwm.set fan 10; gpio.toggle relay"), "the verbs chain with ;");
    CHECK(!parses("gpio.set relay maybe"), "gpio.set refuses an unknown level word");
    CHECK(!parses("gpio.set relay"), "gpio.set refuses a missing value");
    CHECK(!parses("gpio.toggle"), "gpio.toggle refuses a missing pin");
    CHECK(!parses("gpio.toggle relay extra"), "gpio.toggle refuses a trailing argument");
    CHECK(!parses("pwm.set fan 101") && !parses("pwm.set fan -1") && !parses("pwm.set fan abc") && !parses("pwm.set fan"),
          "pwm.set refuses out of range, non-numbers and a missing duty");
    CHECK(!parses("gpio.set abcdefghijabcdefghijabcdefghijabc on"), "a pin token longer than 31 is refused, not cut");

    // ── the firmware's pin check runs at save time ──
    simple_rules_set_io_hooks(on_check, on_run);
    CHECK(parses("gpio.set relay on") && parses("gpio.set 21 on") && parses("pwm.set fan 5"), "configured pins pass the check");
    uint16_t id = 0;
    CHECK(!simple_rules_add("x", "ON io#p DO gpio.set lamp on ENDON", &id) &&
          std::strstr(dsl_last_error(), "unknown pin 'lamp' (outputs: relay)") != nullptr,
          "an unconfigured pin is refused with the firmware's words");
    CHECK(!simple_rules_add("x", "ON io#p DO pwm.set relay 5 ENDON", &id) &&
          std::strstr(dsl_last_error(), "unknown pin 'relay' (pwm: fan)") != nullptr, "pwm.set checks against pwm pins");
    CHECK(!simple_rules_add("x", "ON io#p DO gpio.toggle fan ENDON", &id), "gpio.toggle checks against outputs");

    // ── fire: what the run hook receives ──
    run_reset();
    const uint16_t r1 = add("ON io#a DO gpio.set relay on ENDON");
    publish_attr("a", 1); drain();
    CHECK(r1 && g_run.calls == 1 && g_run.op == SimpleRulesIoOp::SET && std::strcmp(g_run.pin, "relay") == 0 &&
          std::strcmp(g_run.value, "on") == 0, "gpio.set relay on -> run(SET, relay, on)");

    run_reset();
    add("ON io#b DO gpio.set relay %value% ENDON");
    publish_attr("b", 1); drain();
    CHECK(g_run.calls == 1 && std::strcmp(g_run.value, "1") == 0, "%value% passes the trigger value through");

    run_reset();
    add("ON io#c DO pwm.set fan %value%/2 ENDON");
    publish_attr("c", 51); drain();
    CHECK(g_run.calls == 1 && g_run.op == SimpleRulesIoOp::PWM && std::strcmp(g_run.pin, "fan") == 0 &&
          std::strcmp(g_run.value, "25") == 0, "pwm.set expression is evaluated per fire");

    run_reset();
    add("ON io#d DO gpio.toggle relay ENDON");
    publish_attr("d", 1); drain();
    CHECK(g_run.calls == 1 && g_run.op == SimpleRulesIoOp::TOGGLE && std::strcmp(g_run.pin, "relay") == 0 &&
          g_run.value[0] == '\0', "gpio.toggle -> run(TOGGLE, relay, \"\")");

    run_reset(); g_run.result = false;
    const uint16_t r5 = add("ON io#e DO gpio.set 21 off ENDON");
    publish_attr("e", 1); drain();
    CHECK(g_run.calls == 1 && last_skip_is(r5, "action_error:gpio.set"), "a refused write is an action error in the rule status");

    // ── a pin renamed or removed after the rule was saved: the rule stays, fails at fire time (spec §3) ──
    run_reset();
    const uint16_t r7 = add("ON io#g DO gpio.set relay on ENDON");
    g_refuse_all = true;
    simple_rules_reload();   // what the firmware does after every pin-config save
    run_reset(); g_run.result = false;
    publish_attr("g", 1); drain();
    CHECK(r7 && g_run.calls == 1 && last_skip_is(r7, "action_error:gpio.set"),
          "a reload keeps a rule whose pin is gone; it fails at fire time as an action error");
    g_refuse_all = false;

    simple_rules_set_io_hooks(nullptr, nullptr);
    run_reset();
    const uint16_t r6 = add("ON io#f DO pwm.set fan 50 ENDON");
    publish_attr("f", 1); drain();
    CHECK(g_run.calls == 0 && last_skip_is(r6, "action_error:pwm.set"), "no hook registered: action error, nothing runs");

    if (s_failures) { printf("%d check(s) failed\n", s_failures); return 1; }
    printf("gpio actions: all checks passed\n");
    return 0;
}
