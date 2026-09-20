// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// device_cmd -- set one attribute of a device, the same way from every door.
//
// Before this file each transport had its own copy of the same twenty lines
// (pool lookup, value dispatch, optimistic shadow) and they drifted: REST
// truncated decimals the WebSocket accepted, group fan-out skipped the shadow,
// the P4's HAP handler had its own rules. Now REST, WebSocket, MQTT, HAP, Lua
// and the rule engine call one function and get one set of results.
//
// Contract:
//   * the device pool is read under its lock and released BEFORE the radio is
//     touched (a slow send must never stall device reports);
//   * bool / integer / decimal / string go to the converter as such -- the
//     converter owns scaling; a converter that only takes integers refuses a
//     decimal and that comes back as DEVCMD_NO_CONVERTER, never a truncation;
//   * on success the shadow gets an optimistic value: "state" -> VAL_BOOL,
//     other integers -> VAL_INT, decimals -> VAL_FLOAT x100, strings -> none
//     (the shadow holds numbers; the device's report supplies the rest).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVCMD_BOOL = 0,
    DEVCMD_INT,      // any integral number (negative allowed)
    DEVCMD_FLOAT,    // a number with a fraction
    DEVCMD_STR,      // an enum option or other text
} DevCmdKind;

typedef struct {
    DevCmdKind  kind;
    bool        b;
    int64_t     i;
    double      f;
    const char* s;   // valid for the call only
} DevCmdValue;

typedef enum {
    DEVCMD_OK = 0,
    DEVCMD_BAD_ARGS,       // no key, no value
    DEVCMD_BAD_VALUE,      // value kind not one of the four (JSON null, object, array)
    DEVCMD_NOT_FOUND,      // no such device in the pool
    DEVCMD_NO_CONVERTER,   // no converter claims the key for this device, or it refused the value
    DEVCMD_BAD_NAME,       // rename: empty, longer than 29 bytes, or holds a quote, backslash or control character
    DEVCMD_RADIO,          // the radio refused (permit join)
} DevCmdResult;

// The words every transport answers with. Stable: the web UI and the docs use them.
const char* device_cmd_result_str(DevCmdResult r);

// `ep` 0 = the device's first endpoint (its default).
DevCmdResult device_cmd_set_attr(uint64_t ieee, uint8_t ep, const char* key, const DevCmdValue* v);

static inline DevCmdValue device_cmd_bool(bool b)         { DevCmdValue v = {DEVCMD_BOOL, b, 0, 0.0, 0}; return v; }
static inline DevCmdValue device_cmd_int(int64_t i)       { DevCmdValue v = {DEVCMD_INT, false, i, 0.0, 0}; return v; }
static inline DevCmdValue device_cmd_float(double f)      { DevCmdValue v = {DEVCMD_FLOAT, false, 0, f, 0}; return v; }
static inline DevCmdValue device_cmd_str(const char* s)   { DevCmdValue v = {DEVCMD_STR, false, 0, 0.0, s}; return v; }
// A JSON number: integral values become DEVCMD_INT, others DEVCMD_FLOAT.
DevCmdValue device_cmd_number(double d);

// ── Rename ──────────────────────────────────────────────────────────────
// Validates (device.list rows are built with snprintf, so one quote or
// backslash in a name would break the whole list), writes the pool, persists
// at HIGH priority, then calls the changed hook.
DevCmdResult device_cmd_rename(uint64_t ieee, const char* name);

// What must follow a rename lives in the composition root, not here: the rule
// engine re-resolves names (simple_rules depends on this component, so it
// cannot be called from it) and Home Assistant learns the new name where that
// bridge exists. Register once at boot; one hook is enough.
typedef void (*DevCmdChangedFn)(uint64_t ieee);
void device_cmd_set_changed_hook(DevCmdChangedFn fn);

// ── Permit join ─────────────────────────────────────────────────────────
// `secs` 0 closes the window. 255 means "forever" on the air and is clamped
// to 254 (the longest bounded window) so no transport can leave the network
// open with no auto-close. The deadline is kept here so REST and WebSocket
// agree on whether the window is open.
DevCmdResult device_cmd_permit_join(uint8_t secs);
void device_cmd_permit_join_status(bool* open, int* remaining_s);

// ── Remove ──────────────────────────────────────────────────────────────
// Soft: ask the device to leave, tombstone it in the pool and persist the
// mark, so lists hide it and a rejoin keeps its name. Hard: leave, then wipe
// the pool slot, the shadow, the converter caches and the stored row; every
// step is idempotent, so hard on an unknown IEEE still returns OK (it cleans
// leftovers of a device the pool no longer has).
DevCmdResult device_cmd_remove(uint64_t ieee, bool hard);

#ifdef __cplusplus
}  // extern "C"
#endif
