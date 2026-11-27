// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// How much the hub lets the cloud do (review 2026-11-26 HL-03, ruling R18), set on the hub's own page only:
// view = reads; devices = + device and group control; full = + rules, the join window, logs, script reads.
typedef enum { REMOTE_LEVEL_VIEW = 0, REMOTE_LEVEL_DEVICES = 1, REMOTE_LEVEL_FULL = 2 } remote_level_t;

// The hub's settings for one cloud command: its level, "Allow script changes from the cloud" and "Allow the cloud
// to disarm the alarm" (HL-02, R19). The relay reads them for every command, so a change applies to the next one.
typedef struct { remote_level_t level; bool scripts_on; bool alarm_on; } remote_policy_t;

// The relay's answer to one cloud command: nullptr = pass it to the dispatcher, else the error code:
//   "local_only"      settings.set carrying a key only the hub's own page may change (remote_is_local_only_key),
//                     whatever the build trusts the cloud with;
//   "cmd_not_allowed" a command the relay never passes (and every command when `p` is null);
//   "level_view"      a command above the view level while the level is view (a level outside the enum counts
//                     as view), "level_devices" one above devices while it is devices (HL-03);
//   "scripts_off"     a switch-gated op at full while the script switch is off. The switch gates changing Lua
//                     and engine control (script.write, script.reload, system.restart), a direct script.run, and
//                     a rule save that adds a script.run action (remote_rule_refusal below).
// `local_only_key`: the args carry a key `remote_is_local_only_key` accepts.
// LINEAR SCANS over static arrays (~70 entries), once per cloud frame.
const char* remote_cmd_refusal(const char* cmd, bool local_only_key, const remote_policy_t* p);

// The relay's answer to a rule save (rule.create / rule.update) whose text is `dsl`, asked once remote_cmd_refusal
// passed: "scripts_off" when the text runs a stored script (a `script.run` action) while the script switch is off --
// the cloud may not run Lua on the hub by way of a new rule either (review 2026-10-24 LU-04); "alarm_off" when an
// action writes to the Alarm device (a zigbee.* action naming it by its IEEE or its name, or a publish to its MQTT
// command topic) while the alarm switch is off (HL-02); else nullptr. A trigger or condition that reads the Alarm
// device passes. A rule already on the hub keeps running on its own triggers; rule.run and an enabling rule.enable
// of one are remote_rule_run_refusal's and remote_rule_enable_refusal's. A null `p` counts as both switches off.
const char* remote_rule_refusal(const char* cmd, const char* dsl, const remote_policy_t* p);

// The relay's answer to a command aimed at device `ieee` (its args.ieee, a member it lists, a group that holds it):
// "alarm_off" when `ieee` is the Alarm device (remote_alarm_ieee(), non-zero), the alarm switch is off and the
// command writes to it, else nullptr (HL-02, R19). Reads pass, and so does the one write that arms: device.attr.set
// of `alarm_state` to armed_<id> or of a profile's own key to armed (zhac-wired-core alarm.cpp local_cmd), when
// remote_alarm_arm_replaces(<id>) says it stops no other profile (final review 2026-11-27 Critical 1).
// `key`, `str_value`: a device.attr.set's args (`str_value` null when the value is not a string). A null `p` counts
// as the switch off.
const char* remote_target_refusal(const char* cmd, uint64_t ieee, const char* key, const char* str_value,
                                  const remote_policy_t* p);

// rule.run of stored rule `id` (HL-02, final review 2026-11-27): "alarm_off" when the alarm switch is off and the
// rule's stored text (remote_rule_text) writes to the Alarm device, read as a rule save is; nullptr otherwise, and
// for a rule that is not stored, so the dispatcher answers as before. A null `p` counts as the switch off.
const char* remote_rule_run_refusal(uint16_t id, const remote_policy_t* p);
// rule.enable of stored rule `id` (re-review 2026-11-27): when `enabling` (its args.enabled, read as cmd_rule_enable
// reads it), remote_rule_run_refusal's answer, since the rule then runs on its own trigger; nullptr for a disabling.
const char* remote_rule_enable_refusal(uint16_t id, bool enabling, const remote_policy_t* p);
// True when stored rule `id` writes to the Alarm device (or its text is too long to read to its end), whatever the
// switches say: the relay raises the owner's alert for such a rule.run or enabling rule.enable it passed (R19).
bool remote_stored_rule_writes_alarm(uint16_t id);

// True when rule text `dsl` writes to the Alarm device (remote_alarm_ieee(), non-zero), read as remote_rule_refusal
// reads it. The relay raises the owner's alert for such a save it passed (R19).
bool remote_rule_writes_alarm(const char* dsl);

// An account id as the cloud sends one in args.by: 1-40 of [0-9a-f-].
bool remote_account_id_ok(const char* by);

// An IEEE in a command's args read as the firmware's dispatcher reads it (zhac-wired-core ws_bridge.cpp
// parse_ieee): an optional 0x/0X, then strtoull base 16, so every spelling the dispatcher takes for a device is
// that device here too. 0 for null.
uint64_t remote_ieee_arg(const char* s);

// True for a settings.set key that only the hub's own page may change; the relay answers settings.set
// carrying one of them (with any non-null value) with local_only. remote_scripts: spec 2026-10-05 §3.1;
// log_to_cloud: spec 2026-10-06 §4.1; remote_level, remote_alarm: review 2026-11-26 HL-03, HL-02.
bool remote_is_local_only_key(const char* key);

// True iff `cmd` passes now: remote_cmd_refusal(cmd, false, <the policy the weak functions below give>) == nullptr.
bool remote_cmd_allowed(const char* cmd);

// The firmware's settings and its Alarm device. Weak defaults: dual-chip and mono define nothing, so the cloud only
// reads there; zhac-wired-core overrides them all (ws_bridge.cpp).
// "Allow script changes from the cloud" (spec 2026-10-05 §3.1). Weak default: false.
bool remote_scripts_allowed(void);
// The cloud-control level (R18). Weak default: view.
remote_level_t remote_level_now(void);
// "Allow the cloud to disarm the alarm" (R19). Weak default: false.
bool remote_alarm_allowed(void);
// The Alarm device's IEEE. Weak default: 0, no Alarm device and nothing to guard.
uint64_t remote_alarm_ieee(void);
// The Alarm device's name, which a rule may use for it in place of its IEEE: into `out` (`cap` bytes, always
// terminated); returns its length. Weak default: "".
size_t remote_alarm_name(char* out, size_t cap);
// True when group `id` lists device `ieee`, so a group.cmd reaches it. Weak default: true (fails closed: a build
// that names an Alarm device but cannot look into its groups treats every group as holding it).
bool remote_group_has(uint16_t id, uint64_t ieee);
// True when arming profile `id` of the Alarm device would stop another profile: an intrusion profile while another
// intrusion profile is armed or arming (zhac-wired-core alarm.cpp alarm_arm_replaces). Weak default: true (fails
// closed: a build that cannot ask refuses every arm with the alarm switch off).
bool remote_alarm_arm_replaces(const char* id);
// The stored text of rule `id` into `out` (`cap` bytes, always terminated); returns its full length, as snprintf
// does, or 0 when no such rule is stored. Weak default: 0.
size_t remote_rule_text(uint16_t id, char* out, size_t cap);
// Called on task_remote after the hub took a cloud write to the Alarm device: a device.attr.set's or group.cmd's
// key and string value (null when not a string), else the command's name and null. `by`: the account id the cloud
// sent when remote_account_id_ok passes it, else "". Weak default: nothing; zhac-wired-core raises a hub alert.
void remote_alarm_changed(const char* key, const char* value, const char* by);

// True iff the event name should be mirrored to the remote sink now: remote_event_allowed_at(name,
// remote_level_now()), read for every event, so a level change applies to the next one.
bool remote_event_allowed(const char* name);
// The same at `level`: a listed event, and for script.added / script.updated / script.error the full level, as
// script.list needs (HL-03, R18): they carry its size, fingerprint and mtime, or Lua's error text, which can quote
// anything a script holds. script.deleted carries the name only. A level outside the enum counts as view.
bool remote_event_allowed_at(const char* name, remote_level_t level);

// True iff the event waits in the send queue while the link is down (sent once it is READY): an
// `alert`, the alarm panel's notification. Every other event is dropped then, because the next
// reconnect's bootstrap supersedes it.
bool remote_event_held_offline(const char* name);

// The hub-log line for a change the cloud relays (§3.7), e.g.
//   "script.write by 00000000-0000-4000-8000-000000000001 name=heating"
// `by` is printed when it looks like an account id (1-40 of [0-9a-f-]), else "-"; ` id=<id>` (rules,
// id >= 0) and ` name=<name>` (only a valid script name) follow when present. False when `cmd` is not a
// logged change (reads, checks, script.part).
bool remote_change_log_line(const char* cmd, const char* by, long id, const char* name, char* out, size_t cap);

// A name the hub stores a script under: [A-Za-z0-9_-]{1,24}.
bool remote_script_name_ok(const char* name);

#ifdef __cplusplus
}
#endif
