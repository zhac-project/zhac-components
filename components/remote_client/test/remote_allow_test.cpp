// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_allow.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static bool is(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

// The firmware's overrides of the Alarm device and its name, as zhac-wired-core defines them (ws_bridge.cpp). The
// level and the two switches keep their weak defaults here (weakDefaultsAreViewAndAlarmOff).
static constexpr uint64_t kAlarm = 0x025A484143414C00ULL;   // zhac-wired-core alarm.h kAlarmIeee
static constexpr uint64_t kLamp  = 0x00124B0001020304ULL;
static uint64_t g_alarm = kAlarm;                           // 0: a build with no Alarm device
static const char* g_alarm_name = "Alarm";
extern "C" uint64_t remote_alarm_ieee(void) { return g_alarm; }
extern "C" size_t remote_alarm_name(char* out, size_t cap) {
    return static_cast<size_t>(std::snprintf(out, cap, "%s", g_alarm_name));
}

static constexpr remote_level_t V = REMOTE_LEVEL_VIEW, D = REMOTE_LEVEL_DEVICES, F = REMOTE_LEVEL_FULL;
static remote_policy_t pol(remote_level_t level, bool scripts_on = false, bool alarm_on = false) {
    return remote_policy_t{level, scripts_on, alarm_on};
}

// One refusal (or none) as the case wants it; prints the case before the assert stops the run.
static void expect(const char* got, const char* want, const char* what, const char* cmd, const remote_policy_t& p) {
    const bool ok = want ? is(got, want) : got == nullptr;
    if (!ok)
        std::printf("%s: %s at level %d (scripts %d, alarm %d): %s, want %s\n", what, cmd, static_cast<int>(p.level),
                    p.scripts_on, p.alarm_on, got ? got : "passed", want ? want : "passed");
    assert(ok);
}

// Every command the relay passes and the cloud-control level it needs (review 2026-11-26 HL-03, ruling R18).
struct Need { const char* cmd; remote_level_t level; };
static const Need kNeeds[] = {
    // view: the reads
    {"status.get", V}, {"alerts.get", V}, {"wifi.status", V}, {"wifi.scan", V}, {"zigbee.permit_join.status", V},
    {"device.list", V}, {"device.get", V}, {"device.groups.list", V}, {"rule.list", V}, {"rules.status", V},
    {"uplink.get", V}, {"rainmaker.status", V}, {"device.rainmaker.list", V}, {"group.list", V}, {"group.get", V},
    // devices: device and group control
    {"device.bind", D}, {"device.rename", D}, {"device.attr.set", D}, {"device.options.set", D},
    {"device.reinterview", D}, {"device.configure", D}, {"device.groups.add", D}, {"device.groups.remove", D},
    {"device.groups.refresh", D}, {"group.create", D}, {"group.update", D}, {"group.delete", D}, {"group.cmd", D},
    {"rainmaker.assoc.set", D}, {"device.rainmaker.add", D}, {"device.rainmaker.remove", D},
    // full: rules, pairing, logs, script reads and deletes
    {"rule.create", F}, {"rule.delete", F}, {"rule.enable", F}, {"rule.update", F}, {"rule.run", F},
    {"rule.check", F}, {"script.part", F}, {"zigbee.permit_join", F}, {"logs.get", F},
    {"diagnostics.unhandled.get", F}, {"script.list", F}, {"script.read", F}, {"script.delete", F},
    {"script.check", F},
};

static void levelTable() {
    for (const Need& n : kNeeds)
        for (remote_level_t l : {V, D, F})
            for (bool scripts : {false, true})
                for (bool alarm : {false, true}) {
                    const remote_policy_t p = pol(l, scripts, alarm);
                    const char* want = l >= n.level ? nullptr : l == V ? "level_view" : "level_devices";
                    expect(remote_cmd_refusal(n.cmd, false, &p), want, "level table", n.cmd, p);
                }
    // A level the enum does not name (a damaged setting) counts as view, never as more than full.
    const remote_policy_t odd{static_cast<remote_level_t>(3), true, true};
    expect(remote_cmd_refusal("device.attr.set", false, &odd), "level_view", "unknown level", "device.attr.set", odd);
    expect(remote_cmd_refusal("status.get", false, &odd), nullptr, "unknown level", "status.get", odd);
    assert(is(remote_cmd_refusal("status.get", false, nullptr), "cmd_not_allowed"));   // no policy: nothing passes
}

// The script switch keeps its rule (spec 2026-10-05 §3.1) and also needs full.
static void scriptSwitchAtFull() {
    for (const char* c : {"script.write", "script.run", "script.reload", "system.restart"}) {
        for (bool scripts : {false, true}) {
            remote_policy_t p = pol(V, scripts);
            expect(remote_cmd_refusal(c, false, &p), "level_view", "script switch", c, p);
            p = pol(D, scripts);
            expect(remote_cmd_refusal(c, false, &p), "level_devices", "script switch", c, p);
        }
        remote_policy_t p = pol(F, false);
        expect(remote_cmd_refusal(c, false, &p), "scripts_off", "script switch", c, p);
        p = pol(F, true);
        expect(remote_cmd_refusal(c, false, &p), nullptr, "script switch", c, p);
    }

    // A rule that runs a stored script is Lua from the cloud like script.run (review LU-04): with the switch off a
    // save of one is refused scripts_off; rules without that action, other commands, and an open switch pass.
    const char* runs = "ON lamp#state DO script.run heating ENDON";
    const char* plain = "ON lamp#state DO zigbee.toggle lamp state ENDON";
    const remote_policy_t off = pol(F, false), on = pol(F, true);
    for (const char* c : {"rule.create", "rule.update"}) {
        assert(is(remote_rule_refusal(c, runs, &off), "scripts_off"));
        assert(remote_rule_refusal(c, runs, &on) == nullptr);
        assert(remote_rule_refusal(c, plain, &off) == nullptr);
        assert(remote_rule_refusal(c, nullptr, &off) == nullptr);
    }
    assert(is(remote_rule_refusal("rule.create", "ON x#y DO log a; script.run b ENDON", &off), "scripts_off"));
    for (const char* c : {"rule.check", "rule.run", "rule.enable", "status.get"}) {   // these store no rule text
        assert(remote_rule_refusal(c, runs, &off) == nullptr);
    }
}

// The destructive set stays privileged: no level and no switch opens it (denied without the Kconfig).
static void privilegedStillRefused() {
    for (const char* c : {"ota.s3", "ota.p4", "zigbee.reset", "device.delete", "settings.set",
                          "system.token.rotate", "uplink.set", "deploy.apply", "deploy.etag"}) {
        for (remote_level_t l : {V, D, F}) {
            const remote_policy_t p = pol(l, true, true);
            expect(remote_cmd_refusal(c, false, &p), "cmd_not_allowed", "privileged", c, p);
        }
    }
    const remote_policy_t all = pol(F, true, true);
    // The wifi mutators (orphan-the-device risk), the remote.* admin family, the alarm panel's own commands (not on
    // the cloud link) and unknown or typo'd commands are never relayed.
    for (const char* c : {"wifi.connect", "wifi.disconnect", "remote.connect", "remote.disconnect", "remote.status",
                          "remote.auth", "alarm.get", "alarm.arm", "alarm.disarm", "alarm.silence", "alarm.config.set",
                          "", "not.a.command", "device.lis", "device.listing"}) {
        expect(remote_cmd_refusal(c, false, &all), "cmd_not_allowed", "never relayed", c, all);
    }
    assert(is(remote_cmd_refusal(nullptr, false, &all), "cmd_not_allowed"));
}

// The settings only the hub's own page may change: settings.set carrying one is local_only at every level, with
// every switch. The level and the alarm switch are such settings (HL-02, HL-03).
static void localOnlyKeys() {
    for (remote_level_t l : {V, D, F}) {
        for (bool on : {false, true}) {
            const remote_policy_t p = pol(l, on, on);
            expect(remote_cmd_refusal("settings.set", true, &p), "local_only", "local only", "settings.set", p);
        }
    }
    const remote_policy_t p = pol(V);
    expect(remote_cmd_refusal("status.get", true, &p), nullptr, "local only", "status.get", p);   // only settings.set
    for (const char* k : {"remote_scripts", "log_to_cloud", "remote_level", "remote_alarm"}) assert(remote_is_local_only_key(k));
    for (const char* k : {"timezone", "", "remote_levels", "remote"}) assert(!remote_is_local_only_key(k));
    assert(!remote_is_local_only_key(nullptr));
}

// ── The alarm switch (review 2026-11-26 HL-02, ruling R19) ──

// Every command that writes to a device it names by args.ieee, and to each member of a group it creates or updates.
static const char* const kDeviceWrites[] = {
    "device.attr.set", "device.options.set", "device.rename", "device.bind", "device.reinterview", "device.configure",
    "device.groups.add", "device.groups.remove", "device.groups.refresh", "device.rainmaker.add",
    "device.rainmaker.remove", "group.create", "group.update",
};

static void alarmDisarmRefusedWhenOff() {
    for (remote_level_t l : {D, F}) {
        const remote_policy_t p = pol(l, true, false);
        assert(is(remote_target_refusal("device.attr.set", kAlarm, "alarm_state", "disarmed", &p), "alarm_off"));
        assert(is(remote_target_refusal("device.attr.set", kAlarm, "night", "disarmed", &p), "alarm_off"));
    }
}

static void alarmArmAllowedWhenOff() {
    const remote_policy_t p = pol(D, false, false);
    // alarm_state takes armed_<id> of an intrusion profile, a profile's own attribute takes armed (alarm.cpp).
    assert(remote_target_refusal("device.attr.set", kAlarm, "alarm_state", "armed_away", &p) == nullptr);
    assert(remote_target_refusal("device.attr.set", kAlarm, "night", "armed", &p) == nullptr);
    // Reading the device stays open.
    for (const char* c : {"device.get", "device.groups.list", "device.list", "device.rainmaker.list"})
        assert(remote_target_refusal(c, kAlarm, nullptr, nullptr, &p) == nullptr);
}

static void alarmOptionsRefusedWhenOff() {
    const remote_policy_t p = pol(F, true, false);
    for (const char* c : kDeviceWrites) {
        if (std::strcmp(c, "device.attr.set") == 0) continue;   // its keys below
        expect(remote_target_refusal(c, kAlarm, nullptr, nullptr, &p), "alarm_off", "alarm write", c, p);
        expect(remote_target_refusal(c, kAlarm, "alarm_state", "armed_away", &p), "alarm_off", "alarm write", c, p);
    }
    // device.attr.set of anything but an arming value: the panel's own states, the arming value of the other kind
    // of key, another case, a value that is not text (a boolean or a number arrives as null), no key.
    struct KV { const char* key; const char* value; };
    for (const KV& kv : {KV{"alarm_state", "pending"}, KV{"alarm_state", "triggered"}, KV{"alarm_state", "arming"},
                         KV{"alarm_state", "armed"}, KV{"night", "armed_away"}, KV{"alarm_state", "Armed_away"},
                         KV{"night", "ARMED"}, KV{"alarm_state", nullptr}, KV{nullptr, "armed"},
                         KV{"alarm_sensor", "disarmed"}}) {
        assert(is(remote_target_refusal("device.attr.set", kAlarm, kv.key, kv.value, &p), "alarm_off"));
    }
}

static void alarmAnyWriteAllowedWhenOn() {
    const remote_policy_t p = pol(F, false, true);
    for (const char* c : kDeviceWrites) {
        expect(remote_target_refusal(c, kAlarm, nullptr, nullptr, &p), nullptr, "alarm switch on", c, p);
    }
    assert(remote_target_refusal("device.attr.set", kAlarm, "alarm_state", "disarmed", &p) == nullptr);
    assert(remote_target_refusal("device.attr.set", kAlarm, "night", "disarmed", &p) == nullptr);
    const char* disarm = "ON Time#Cron=0 7 * * * DO zigbee.set Alarm alarm_state disarmed ENDON";
    assert(remote_rule_refusal("rule.create", disarm, &p) == nullptr);
}

static void otherDevicesUnaffected() {
    const remote_policy_t p = pol(F, false, false);
    for (const char* c : kDeviceWrites) {
        expect(remote_target_refusal(c, kLamp, "state", "OFF", &p), nullptr, "another device", c, p);
        expect(remote_target_refusal(c, 0, nullptr, nullptr, &p), nullptr, "no device", c, p);
    }
    assert(remote_target_refusal("device.attr.set", kLamp, "alarm_state", "disarmed", &p) == nullptr);
    // A build without an Alarm device (remote_alarm_ieee() 0) protects nothing.
    g_alarm = 0;
    assert(remote_target_refusal("device.attr.set", kAlarm, "alarm_state", "disarmed", &p) == nullptr);
    assert(remote_rule_refusal("rule.create", "ON a#b DO zigbee.set Alarm alarm_state disarmed ENDON", &p) == nullptr);
    g_alarm = kAlarm;
}

// A rule save whose action writes to the Alarm device: every way the rules engine names it there (an 0x IEEE that
// strtoull reads, the device's friendly name), in any action, however spaced.
static void ruleActionOnAlarmRefusedWhenOff() {
    const remote_policy_t p = pol(F, true, false);
    for (const char* c : {"rule.create", "rule.update"}) {
        for (const char* dsl : {
                 "ON Time#Cron=0 7 * * * DO zigbee.set Alarm alarm_state disarmed ENDON",
                 "ON lamp#state DO zigbee.set 0x025A484143414C00 night disarmed ENDON",
                 "ON lamp#state DO zigbee.set 0X025a484143414c00 alarm_state armed_away ENDON",   // arming too
                 "ON lamp#state DO zigbee.set 0x25A484143414C00 alarm_state disarmed ENDON",
                 "ON lamp#state DO zigbee.set 0x0000025A484143414C00 alarm_state disarmed ENDON",
                 "ON lamp#state DO zigbee.set 0x025A484143414C00zz alarm_state disarmed ENDON",
                 "ON lamp#state DO zigbee.set 0x025A484143414C00\talarm_state disarmed ENDON",
                 "ON lamp#state DO log hi;\tzigbee.toggle Alarm night ENDON",
                 "ON lamp#state DO zigbee.set lamp state ON; zigbee.set   Alarm alarm_state disarmed ENDON",
                 "  ON  lamp#state DO zigbee.toggle Alarm night;ENDON",
                 "ON lamp#state DO zigbee.set 0x25A484143414C00ENDON",   // the action ends at ENDON, not past it
                 "ON lamp#state DO zigbee.set 0x00000000000000000000000000025A484143414C00 x y ENDON"}) {   // too long
            if (!is(remote_rule_refusal(c, dsl, &p), "alarm_off")) std::printf("not refused: %s\n", dsl);
            assert(is(remote_rule_refusal(c, dsl, &p), "alarm_off"));
        }
    }
    // The check follows the device's name: renamed, the old name is another device's (or none).
    g_alarm_name = "Siren";
    assert(is(remote_rule_refusal("rule.create", "ON a#b DO zigbee.set Siren alarm_state disarmed ENDON", &p),
              "alarm_off"));
    assert(remote_rule_refusal("rule.create", "ON a#b DO zigbee.set Alarm alarm_state disarmed ENDON", &p) == nullptr);
    g_alarm_name = "Alarm";
}

static void ruleTriggerOnAlarmAllowed() {
    const remote_policy_t p = pol(F, false, false);
    for (const char* dsl : {"ON Alarm#alarm_state=triggered DO zigbee.set Siren state ON ENDON",
                            "ON 0x025A484143414C00#night=armed DO publish home/alarm armed ENDON",
                            "ON Alarm DO log Alarm changed ENDON",   // a text naming it writes nothing
                            "ON Offline#Alarm DO event alarm_gone ENDON",
                            "ON Event#alarm_triggered_away DO zigbee.toggle lamp state ENDON"}) {
        if (remote_rule_refusal("rule.create", dsl, &p)) std::printf("refused: %s\n", dsl);
        assert(remote_rule_refusal("rule.create", dsl, &p) == nullptr);
        assert(remote_rule_refusal("rule.update", dsl, &p) == nullptr);
    }
}

static void unrelatedRuleAllowed() {
    const remote_policy_t p = pol(F, false, false);
    for (const char* dsl : {"ON lamp#state DO zigbee.toggle lamp state ENDON",
                            "ON lamp#state DO zigbee.set 0x00124B0001020304 state ON ENDON",
                            "ON lamp#state DO zigbee.set Alarmlight state ON ENDON",
                            "ON lamp#state DO gpio.set led on; publish a/b c ENDON"}) {
        assert(remote_rule_refusal("rule.create", dsl, &p) == nullptr);
    }
    // Commands that store no rule text are not asked.
    const char* disarm = "ON a#b DO zigbee.set Alarm alarm_state disarmed ENDON";
    for (const char* c : {"rule.check", "rule.run", "rule.enable", "status.get"})
        assert(remote_rule_refusal(c, disarm, &p) == nullptr);
}

static void weakDefaultsAreViewAndAlarmOff() {
    assert(remote_level_now() == REMOTE_LEVEL_VIEW);
    assert(!remote_alarm_allowed());
    assert(!remote_scripts_allowed());
    assert(remote_group_has(1, kAlarm));                   // no group lookup: every group may hold the Alarm
    remote_alarm_changed("alarm_state", "disarmed", "");   // a no-op without the firmware
    // So with nothing overridden the relay passes the reads only.
    assert(remote_cmd_allowed("status.get"));
    assert(remote_cmd_allowed("device.list"));
    for (const char* c : {"device.attr.set", "group.cmd", "rule.create", "logs.get", "script.read", "script.write"})
        assert(!remote_cmd_allowed(c));
}

// The IEEE in a command's args, read as the dispatcher reads it (ws_bridge.cpp parse_ieee): every spelling the
// dispatcher takes for the Alarm's IEEE is the Alarm's IEEE here too.
static void ieeeArgReadAsTheDispatcher() {
    char neg[40];
    std::snprintf(neg, sizeof(neg), "0x-%llX", static_cast<unsigned long long>(0ULL - kAlarm));
    for (const char* s : {"0x025A484143414C00", "0X025a484143414c00", "025A484143414C00", "0x25A484143414C00",
                          " 0x025A484143414C00", "0x0x025A484143414C00", "0x025A484143414C00 trailing",
                          static_cast<const char*>(neg)}) {
        if (remote_ieee_arg(s) != kAlarm) std::printf("not the Alarm: '%s'\n", s);
        assert(remote_ieee_arg(s) == kAlarm);
    }
    assert(remote_ieee_arg(nullptr) == 0);
    assert(remote_ieee_arg("") == 0);
    assert(remote_ieee_arg("zz") == 0);
}

// The hub's pushes to the cloud: device.* / rule.* / group.* / alert, the single-chip names, the script switch's
// hub.caps and the cloud logs go at every level. The script mirror's events that carry what only script.list shows
// (size, fingerprint, mtime) or Lua's error text need full, as script.list does (R18); script.deleted has the name only.
static void eventsByLevel() {
    for (const char* e : {"device.added", "device.attribute_change", "rule.fired", "group.changed", "alert",
                          "attr.changed", "attr.bulk", "rule.updated", "rule.deleted", "group.updated",
                          "group.deleted", "script.deleted", "hub.caps", "log.batch", "log.tail"}) {
        for (remote_level_t l : {V, D, F}) assert(remote_event_allowed_at(e, l));
        assert(remote_event_allowed(e));   // at the weak default level, view
    }
    for (const char* e : {"script.added", "script.updated", "script.error"}) {
        assert(remote_event_allowed_at(e, F));
        assert(!remote_event_allowed_at(e, D));
        assert(!remote_event_allowed_at(e, V));
        assert(!remote_event_allowed_at(e, static_cast<remote_level_t>(7)));   // outside the enum: view
        assert(!remote_event_allowed(e));
    }
    // log / wifi / mqtt / bulk events stay LAN-only at every level; unknown ones too.
    for (const char* e : {"log.line", "log.level", "wifi.connected", "mqtt.connected", "bulk.state", "", "nonsense",
                          "script.added.x"}) {
        for (remote_level_t l : {V, D, F}) assert(!remote_event_allowed_at(e, l));
        assert(!remote_event_allowed(e));
    }
    assert(!remote_event_allowed_at(nullptr, F));
    assert(!remote_event_allowed(nullptr));
}

int main() {
    levelTable();
    scriptSwitchAtFull();
    privilegedStillRefused();
    localOnlyKeys();
    alarmDisarmRefusedWhenOff();
    alarmArmAllowedWhenOff();
    alarmOptionsRefusedWhenOff();
    alarmAnyWriteAllowedWhenOn();
    otherDevicesUnaffected();
    ruleActionOnAlarmRefusedWhenOff();
    ruleTriggerOnAlarmAllowed();
    unrelatedRuleAllowed();
    weakDefaultsAreViewAndAlarmOff();
    ieeeArgReadAsTheDispatcher();
    eventsByLevel();

    // An alert (the alarm panel's) waits for the link; every other event is superseded by the next
    // reconnect's bootstrap and is dropped while the link is down.
    assert(remote_event_held_offline("alert"));
    for (const char* e : {"attr.changed", "attr.bulk", "device.added", "alarm.changed", "", "alerts"}) {
        assert(!remote_event_held_offline(e));
    }
    assert(!remote_event_held_offline(nullptr));

    // Script names: [A-Za-z0-9_-]{1,24}.
    assert(remote_script_name_ok("heating"));
    assert(remote_script_name_ok("Door_log-2"));
    assert(remote_script_name_ok("abcdefghijklmnopqrstuvwx"));     // 24
    assert(!remote_script_name_ok("abcdefghijklmnopqrstuvwxy"));   // 25
    assert(!remote_script_name_ok(""));
    assert(!remote_script_name_ok(nullptr));
    assert(!remote_script_name_ok("a b"));
    assert(!remote_script_name_ok("x.lua"));
    assert(!remote_script_name_ok("\xd0\xb6"));

    // The hub-log line for a relayed change (§3.7).
    char line[160];
    assert(remote_change_log_line("script.write", "00000000-0000-4000-8000-000000000001", -1, "heating",
                                  line, sizeof line));
    assert(std::strcmp(line, "script.write by 00000000-0000-4000-8000-000000000001 name=heating") == 0);
    assert(remote_change_log_line("rule.update", "00000000-0000-4000-8000-000000000001", 12, "Porch light",
                                  line, sizeof line));
    assert(std::strcmp(line, "rule.update by 00000000-0000-4000-8000-000000000001 id=12") == 0);
    assert(remote_change_log_line("system.restart", "", -1, "", line, sizeof line));
    assert(std::strcmp(line, "system.restart by -") == 0);
    // `by` that is not an account id is never printed (no forged log lines).
    assert(remote_change_log_line("script.run", "x\nI (1) cloud: fake", -1, "heating", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=heating") == 0);
    assert(remote_change_log_line("script.run", "00000000-0000-4000-8000-00000000000100000", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=a") == 0);      // 41 characters
    assert(remote_change_log_line("script.run", "0123456789abcdef0123456789abcdef01234567", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by 0123456789abcdef0123456789abcdef01234567 name=a") == 0);   // 40 is fine
    assert(remote_change_log_line("script.run", "ABCDEF", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=a") == 0);      // only lowercase hex, as the cloud sends it
    // Reads, checks and parts are not logged.
    for (const char* c : {"rule.list", "rule.check", "script.read", "script.check", "script.part", "status.get"}) {
        assert(!remote_change_log_line(c, "00000000-0000-4000-8000-000000000001", -1, "", line, sizeof line));
    }

    printf("remote_allow_tests: ok\n");
    return 0;
}
