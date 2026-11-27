// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Single source of truth for what the remote channel may touch.
//
// MAINTENANCE: when a command is added to the firmware's dispatcher
// (zhac-wired-core main/ws_bridge.cpp), decide whether the remote
// channel should expose it. If yes, add the cmd name to the list of
// the cloud-control level it needs (kViewCmds / kDevicesCmds /
// kFullCmds below). If no, no change here — the cmd is implicitly
// denied. A command that names a device in args.ieee is checked
// against the Alarm device too (remote_target_refusal).
//
// PERMANENT EXCLUDES (security-driven, do not allow-list):
//   - wifi.connect / wifi.disconnect : orphan-the-device risk.
//   - remote.connect / remote.disconnect / remote.status : admin-only
//     on LAN. Re-exposing them via remote = self foot-gun.

#include "remote_allow.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
// CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED. Nothing included sdkconfig.h here before, so the option never
// took effect (fail-closed). Absent in the host tests.
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

namespace {

// The relayed commands by the cloud-control level they need (review 2026-11-26 HL-03, ruling R18). Each level
// passes its own list and the ones below it.
//
// view: the reads. WiFi mutators are excluded (see above); logs.get and script.* need full.
constexpr const char* kViewCmds[] = {
    "status.get", "alerts.get", "wifi.status", "wifi.scan", "zigbee.permit_join.status",
    "device.list", "device.get", "device.groups.list",
    "rule.list",
    // Cloud automations: per-rule run counters.
    "rules.status",
    "group.list", "group.get",
    "uplink.get", "rainmaker.status", "device.rainmaker.list",
};

// devices: device and group control, every one recoverable (device.delete is privileged).
constexpr const char* kDevicesCmds[] = {
    "device.bind", "device.rename", "device.attr.set", "device.options.set",
    "device.reinterview", "device.configure",
    "device.groups.add", "device.groups.remove", "device.groups.refresh",
    "group.create", "group.update", "group.delete", "group.cmd",
    // RainMaker bridge (Task 18) — recoverable mutations, matching
    // the group.*/device.bind bar: exposing/unexposing a device to
    // RainMaker (add/remove) and starting an association handshake
    // (assoc.set) are all reversible from the app/API with no orphan or
    // destructive-reset risk, same as device.bind or group.delete.
    // uplink.set is deliberately NOT here — see kRemotePrivilegedCmds
    // below: it is a fundamental cloud-uplink mode switch (can force a
    // reboot_required state) closer in kind to settings.set/wifi.connect
    // than to a routine device-management action.
    "rainmaker.assoc.set", "device.rainmaker.add", "device.rainmaker.remove",
};

// full: rules (a stored rule acts on its own, long after the cloud sent it), the join window, the hub log, and
// script reads, deletes and checks.
constexpr const char* kFullCmds[] = {
    "rule.create", "rule.delete", "rule.enable", "rule.update",
    // Cloud automations: "Run now" runs one stored rule's own actions. A rule
    // save that adds a `script.run` action is switch-gated (remote_rule_refusal,
    // review LU-04); a rule already stored keeps running its script, Run now
    // included. The cloud also limits rules with that action to owners and admins.
    "rule.run",
    // Remote editing (spec 2026-10-05 §3.1): a parse check stores nothing, and a
    // staged part reaches flash only through script.write, which the switch gates.
    "rule.check", "script.part",
    // Zigbee: the join window; network reset is privileged.
    "zigbee.permit_join",
    "logs.get", "diagnostics.unhandled.get",
    // Scripts — reading, deleting (runs no code) and checking; changing them and a
    // direct run are switch-gated below
    "script.list", "script.read", "script.delete", "script.check",
};

// Switch-gated (spec 2026-10-05 §3.1): changing Lua and controlling the engine from
// the cloud (script.write, script.reload, system.restart), and a direct script.run.
// They need the full level (HL-03), and then pass only while the hub's own page
// has "Allow script changes from the cloud" on (remote_scripts_allowed());
// otherwise they are refused scripts_off, so the cloud can say "turn it on"
// rather than "not supported". A rule save that adds a
// script.run action is refused the same way (remote_rule_refusal); a rule already
// stored keeps running its script (see "Cloud automations" above).
constexpr const char* kRemoteScriptCmds[] = {
    "script.write", "script.run", "script.reload", "system.restart",
};

// F9 (FINDINGS.md): privileged ops the cloud channel must NOT reach by
// default — firmware flash (ota.*), network wipe (zigbee.reset), device
// removal, and auth/config changes. A compromised or spoofed cloud peer (or
// anyone holding the bearer token) would otherwise gain destructive control.
// Exposed only when CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED is set, for operators
// who explicitly trust the cloud endpoint, and then at the full level. (Lua moved
// to the switch above.)
constexpr const char* kRemotePrivilegedCmds[] = {
    "ota.s3", "ota.p4", "zigbee.reset",
    "device.delete", "settings.set", "system.token.rotate",
    "uplink.set",
};

constexpr const char* kRemoteAllowedEvents[] = {
    // NOTE: these must match the names hap_bridge.cpp actually broadcasts. The live attribute
    // stream is "attr.bulk" (the ~100 ms coalescer) — without it the cloud shadow never sees
    // on/off / sensor changes and can only refresh on reconnect. "device.attribute_change" below
    // is not currently emitted (kept for forward-compat).
    "attr.bulk",
    // The single-chip builds (wired S31/P4, mono) have no coalescer: they push one
    // attr.changed {ieee,key,value,ts} per attribute, and name rule/group edits
    // *.updated / *.deleted. The cloud's LiveEventAdapter takes all of these.
    "attr.changed",
    "device.added", "device.removed", "device.attribute_change",
    "device.online", "device.offline", "device.renamed",
    "device.configure_progress", "device.bound", "device.unbound",
    "rule.added", "rule.removed", "rule.fired", "rule.error",
    "rule.updated", "rule.deleted",
    "group.added", "group.removed", "group.changed",
    "group.updated", "group.deleted",
    "alert",
    // Remote editing (spec 2026-10-05 §3.6): the cloud's script mirror and the switch.
    "script.added", "script.updated", "script.deleted", "script.error", "hub.caps",
    // Cloud logs (spec 2026-10-06 §4.3, §4.4): log batches and, once per run, the restart tail.
    "log.batch", "log.tail",
};

// Of those, the ones only the full level gets, as script.list needs it (HL-03, R18): the script mirror's size,
// fingerprint and mtime, and Lua's error text, which can quote anything a script holds.
constexpr const char* kFullEvents[] = { "script.added", "script.updated", "script.error" };

// settings.set keys only the hub's own page may change (the relay answers local_only): the level and the two
// switches that set what the cloud may do (HL-03, HL-02), and whether the hub's log goes to it.
constexpr const char* kRemoteLocalOnlyKeys[] = { "remote_scripts", "log_to_cloud", "remote_level", "remote_alarm" };

// Changes the hub logs with the `cloud` tag (spec 2026-10-05 §3.7).
constexpr const char* kLoggedCmds[] = {
    "rule.create", "rule.update", "rule.enable", "rule.delete", "rule.run",
    "script.write", "script.delete", "script.run", "script.reload", "system.restart",
};

template <size_t N>
bool listed(const char* const (&list)[N], const char* s) {
    for (const char* x : list) {
        if (std::strcmp(x, s) == 0) return true;
    }
    return false;
}

// An account id as the cloud sends it (a UUID): 1-40 of [0-9a-f-].
bool account_id_ok(const char* by) {
    size_t n = 0;
    for (; by && by[n]; n++) {
        const char c = by[n];
        if (n >= 40 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
    }
    return n > 0;
}

// The level `cmd` needs, or -1 when the relay never passes it. The gated lists first and the levels from the
// top, so a command listed twice by mistake needs the most (fails closed).
int level_needed(const char* cmd) {
    if (listed(kRemoteScriptCmds, cmd)) return REMOTE_LEVEL_FULL;
    if (listed(kRemotePrivilegedCmds, cmd)) {
#ifdef CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED
        return REMOTE_LEVEL_FULL;   // F9: only reachable when the operator explicitly trusts the cloud peer.
#else
        return -1;
#endif
    }
    if (listed(kFullCmds, cmd)) return REMOTE_LEVEL_FULL;
    if (listed(kDevicesCmds, cmd)) return REMOTE_LEVEL_DEVICES;
    if (listed(kViewCmds, cmd)) return REMOTE_LEVEL_VIEW;
    return -1;
}

// simple_rules dsl_parser.cpp skip_ws: the blanks between the DSL's words.
const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// True when an action of rule text `dsl` writes to the Alarm device (HL-02, R19), read the way simple_rules reads
// it (dsl_parser.cpp dsl_parse, simple_rules.cpp run_action): the actions are the text from the first " DO " after
// "ON " to the next "ENDON", split at ';'; the first word after a zigbee.* verb is its device, an IEEE when it
// starts 0x/0X (strtoull base 16, as the runtime reads it), else a friendly name. Triggers and conditions are not
// read. A device word too long for the parser's 31 characters counts as the Alarm: the hub refuses that rule
// anyway, and one that took longer words could reach the Alarm with leading zeros. Text that is no rule is the
// parser's to refuse.
bool rule_writes_alarm(const char* dsl, uint64_t alarm) {
    const char* s = skip_ws(dsl);
    if (std::strncmp(s, "ON ", 3) != 0) return false;
    const char* act = std::strstr(skip_ws(s + 3), " DO ");
    if (!act) return false;
    act += 4;
    const char* const end = std::strstr(act, "ENDON");
    if (!end) return false;
    char name[32], hex[17];
    name[0] = '\0';
    remote_alarm_name(name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(alarm));
    for (const char* a = act; a < end;) {
        const char* semi = static_cast<const char*>(std::memchr(a, ';', static_cast<size_t>(end - a)));
        const char* const stop = semi ? semi : end;
        const char* v = skip_ws(a);
        if (std::strncmp(v, "publish ", 8) == 0) {
            // Final review 2026-11-27 Minor 1: ha_bridge drives a device from <root>/devices/<16 hex digits, either
            // case>/<key>/set (parse_command_topic), so a topic holding the Alarm's IEEE writes to it.
            const char* t = skip_ws(v + 8);
            for (; t + 16 <= stop && *t != ' '; t++)
                if (strncasecmp(t, hex, 16) == 0) return true;
        } else if (std::strncmp(v, "zigbee.", 7) == 0) {   // no blank, ';' or 'E' in it: a match lies inside this action
            while (v < stop && *v != ' ') v++;          // the verb; the parser wants one space after it
            const char* w = v < stop ? skip_ws(v) : stop;
            const char* we = w;
            while (we < stop && *we != ' ') we++;       // copy_token: up to a space or the action's end
            const size_t n = static_cast<size_t>(we - w);
            if (n >= sizeof(name)) return true;
            char ref[sizeof(name)];
            std::memcpy(ref, w, n);
            ref[n] = '\0';
            const bool hex = ref[0] == '0' && (ref[1] == 'x' || ref[1] == 'X');
            if (hex ? std::strtoull(ref, nullptr, 16) == alarm : std::strcmp(ref, name) == 0) return true;
        }
        a = semi ? semi + 1 : end;
    }
    return false;
}

} // namespace

// The firmware's settings and its Alarm device (remote_allow.h). Weak: dual-chip and mono define none, so the cloud
// only reads there.
extern "C" __attribute__((weak)) bool remote_scripts_allowed(void) { return false; }
extern "C" __attribute__((weak)) remote_level_t remote_level_now(void) { return REMOTE_LEVEL_VIEW; }
extern "C" __attribute__((weak)) bool remote_alarm_allowed(void) { return false; }
extern "C" __attribute__((weak)) uint64_t remote_alarm_ieee(void) { return 0; }
extern "C" __attribute__((weak)) size_t remote_alarm_name(char* out, size_t cap) {
    if (out && cap) out[0] = '\0';
    return 0;
}
extern "C" __attribute__((weak)) bool remote_group_has(uint16_t, uint64_t) { return true; }
extern "C" __attribute__((weak)) bool remote_alarm_arm_replaces(const char*) { return true; }
extern "C" __attribute__((weak)) size_t remote_rule_text(uint16_t, char* out, size_t cap) {
    if (out && cap) out[0] = '\0';
    return 0;
}
extern "C" __attribute__((weak)) void remote_alarm_changed(const char*, const char*, const char*) {}

extern "C" const char* remote_cmd_refusal(const char* cmd, bool local_only_key, const remote_policy_t* p) {
    if (!cmd || !*cmd || !p) return "cmd_not_allowed";
    // Only the hub's own page changes these settings, whatever the build trusts the cloud with.
    if (local_only_key && std::strcmp(cmd, "settings.set") == 0) return "local_only";
    const int need = level_needed(cmd);
    if (need < 0) return "cmd_not_allowed";
    // HL-03: below the level the command needs. A level outside the enum (a damaged setting) counts as view.
    const int level = p->level == REMOTE_LEVEL_DEVICES || p->level == REMOTE_LEVEL_FULL ? p->level : REMOTE_LEVEL_VIEW;
    if (level < need) return level == REMOTE_LEVEL_VIEW ? "level_view" : "level_devices";
    if (listed(kRemoteScriptCmds, cmd) && !p->scripts_on) return "scripts_off";
    return nullptr;
}

extern "C" const char* remote_rule_refusal(const char* cmd, const char* dsl, const remote_policy_t* p) {
    if (!cmd || !dsl) return nullptr;
    if (std::strcmp(cmd, "rule.create") != 0 && std::strcmp(cmd, "rule.update") != 0) return nullptr;
    // The rule DSL's only way to Lua is its `script.run <name>` action (simple_rules dsl_parser.cpp,
    // matched case-sensitively); the text anywhere else, say inside a log message, is refused too.
    if (!(p && p->scripts_on) && std::strstr(dsl, "script.run")) return "scripts_off";
    // HL-02: a stored rule acts on its own, so one that writes to the Alarm device needs the alarm switch.
    const uint64_t alarm = remote_alarm_ieee();
    if (alarm && !(p && p->alarm_on) && rule_writes_alarm(dsl, alarm)) return "alarm_off";
    return nullptr;
}

extern "C" bool remote_rule_writes_alarm(const char* dsl) {
    const uint64_t alarm = remote_alarm_ieee();
    return alarm && dsl && rule_writes_alarm(dsl, alarm);
}

extern "C" bool remote_stored_rule_writes_alarm(uint16_t id) {
    if (!remote_alarm_ieee()) return false;
    char dsl[512];   // rule_store keeps 500 bytes of text (zap_common.h RuleSlot)
    const size_t n = remote_rule_text(id, dsl, sizeof(dsl));
    // A text cut short has no ENDON for the check to find: it cannot tell, so it counts as a write.
    return n > 0 && (n >= sizeof(dsl) || remote_rule_writes_alarm(dsl));
}

extern "C" const char* remote_rule_run_refusal(uint16_t id, const remote_policy_t* p) {
    return !(p && p->alarm_on) && remote_stored_rule_writes_alarm(id) ? "alarm_off" : nullptr;
}

extern "C" const char* remote_rule_enable_refusal(uint16_t id, bool enabling, const remote_policy_t* p) {
    return enabling ? remote_rule_run_refusal(id, p) : nullptr;
}

extern "C" bool remote_account_id_ok(const char* by) { return account_id_ok(by); }

extern "C" const char* remote_target_refusal(const char* cmd, uint64_t ieee, const char* key, const char* str_value,
                                             const remote_policy_t* p) {
    if (!cmd || ieee == 0 || ieee != remote_alarm_ieee() || (p && p->alarm_on)) return nullptr;
    if (level_needed(cmd) == REMOTE_LEVEL_VIEW) return nullptr;   // a read
    // R19: the cloud may arm the Alarm device. alarm.cpp local_cmd arms on alarm_state = armed_<id> and on a
    // profile's own key = armed; everything else it takes disarms or changes it. But arming one intrusion profile
    // stops every other one, from Away to a profile that guards less (final review 2026-11-27 Critical 1): only an
    // arm that stops nothing passes.
    if (std::strcmp(cmd, "device.attr.set") == 0 && key && str_value) {
        const bool summary = std::strcmp(key, "alarm_state") == 0;
        if (summary ? std::strncmp(str_value, "armed_", 6) == 0 : std::strcmp(str_value, "armed") == 0)
            return remote_alarm_arm_replaces(summary ? str_value + 6 : key) ? "alarm_off" : nullptr;
    }
    return "alarm_off";
}

extern "C" uint64_t remote_ieee_arg(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return std::strtoull(s, nullptr, 16);
}

extern "C" bool remote_cmd_allowed(const char* cmd) {
    const remote_policy_t p = {remote_level_now(), remote_scripts_allowed(), remote_alarm_allowed()};
    return remote_cmd_refusal(cmd, false, &p) == nullptr;
}

extern "C" bool remote_event_allowed_at(const char* name, remote_level_t level) {
    return name && *name && listed(kRemoteAllowedEvents, name) &&
           (level == REMOTE_LEVEL_FULL || !listed(kFullEvents, name));
}

extern "C" bool remote_event_allowed(const char* name) { return remote_event_allowed_at(name, remote_level_now()); }

extern "C" bool remote_event_held_offline(const char* name) {
    return name && std::strcmp(name, "alert") == 0;
}

extern "C" bool remote_is_local_only_key(const char* key) {
    return key && *key && listed(kRemoteLocalOnlyKeys, key);
}

extern "C" bool remote_script_name_ok(const char* name) {
    size_t n = 0;
    for (; name && name[n]; n++) {
        const char c = name[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok || n >= 24) return false;
    }
    return n > 0;
}

extern "C" bool remote_change_log_line(const char* cmd, const char* by, long id, const char* name,
                                       char* out, size_t cap) {
    if (!cmd || !out || cap == 0 || !listed(kLoggedCmds, cmd)) return false;
    int n = std::snprintf(out, cap, "%s by %s", cmd, account_id_ok(by) ? by : "-");
    if (n > 0 && (size_t)n < cap && id >= 0) n += std::snprintf(out + n, cap - n, " id=%ld", id);
    if (n > 0 && (size_t)n < cap && remote_script_name_ok(name)) {
        n += std::snprintf(out + n, cap - n, " name=%s", name);
    }
    return n > 0;
}
