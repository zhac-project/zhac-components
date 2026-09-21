// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_discovery — Home Assistant MQTT discovery payloads for ZHAC devices.
//
// Pure: no ESP-IDF, no FreeRTOS, no I/O. Takes a device's exposes (the JSON
// array zhac_adapter_build_exposes_json() produces, the same the web UI gets)
// and emits one discovery config per Home Assistant entity. ha_bridge does
// the publishing; this file only decides what to say, so it can be pinned by
// host tests.
//
// Topics (root defaults to "zhac", prefix to "homeassistant"):
//   <prefix>/<component>/zhac_<ieee>_<key>/config   discovery, retained
//   <root>/devices/<IEEE>/<key>                      state, retained, raw value
//   <root>/devices/<IEEE>/<key>/set                  commands from Home Assistant
//   <root>/availability                              "online" / "offline" (LWT)
//   <root>/devices/<IEEE>/availability               battery devices only: "offline"
//                                                    after a day of silence (ha_bridge)
// <ieee> is 16 lower-case hex digits, <IEEE> the same upper-case, as the
// existing <root>/devices/<IEEE>/state topic already uses.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ha {

struct Device {
    uint64_t    ieee;
    const char* name;      // friendly name; the HA device name
    const char* vendor;    // matched definition's vendor, else raw manufacturer
    const char* model;     // matched definition's model, else raw model id
    const char* exposes;   // JSON array of {name,type,access,unit?,category?,values?,value_min?,...}
    bool        battery_powered;   // from the device's power source; a `battery` expose also counts
};

struct Context {
    const char* prefix;     // discovery prefix, "homeassistant"
    const char* root;       // ZHAC MQTT root topic, "zhac"
    const char* bridge_id;  // stable id of this hub on the broker (sanitised root)
};

// One call per entity. `topic` is the config topic; `payload` the JSON config.
using EmitFn = void (*)(const char* component, const char* topic,
                        const char* payload, void* user);

// Emit every discovery config for one device. Returns the entity count, or -1
// when `exposes` is not a JSON array. `passive_out`, when given, says whether
// the device got the per-device availability topic (battery devices do).
//
// Compositions, in this order, each consuming the exposes it uses:
//   local_temperature + a writable heating setpoint   -> climate (+ system_mode, preset,
//                                                        running_state, fan_mode)
//   writable state + brightness (+ color_temp)         -> light
//   writable position and/or state enum OPEN/CLOSE     -> cover (+ tilt)
//   writable lock_state on/off                          -> lock
//   fan_state, or a fan_mode with "off"                 -> fan (+ preset modes)
//   read-only enum "action" with a value list           -> event
// Everything else becomes the single entity its type implies.
int build_device(const Device& d, const Context& c, EmitFn emit, void* user,
                 bool* passive_out = nullptr);

// <root>/devices/<IEEE>/availability. Returns length, or -1 if it does not fit.
int device_availability_topic(char* out, size_t cap, const char* root, uint64_t ieee);

// Emit the hub's own entity (a connectivity sensor on <root>/availability).
// Every device names the hub as its `via_device`, which needs it to exist.
void build_bridge(const Context& c, const char* fw_version, const char* model,
                  EmitFn emit, void* user);

// <root>/devices/<IEEE>/<key>. Returns length, or -1 if it does not fit.
int state_topic(char* out, size_t cap, const char* root, uint64_t ieee, const char* key);

// <prefix>/<component>/zhac_<ieee>_<key>/config. Returns length or -1.
int config_topic(char* out, size_t cap, const char* prefix, const char* component,
                 uint64_t ieee, const char* key);

// Recognise "<root>/devices/<IEEE>/<key>/set". Fills ieee and key; false for
// any other topic, including the device's own state topics.
bool parse_command_topic(const char* topic, size_t len, const char* root,
                         uint64_t* ieee, char* key, size_t key_cap);

// A command payload from Home Assistant as a JSON scalar the firmware's
// attribute setter takes: "1" -> 1, "21.0" -> 21, "21.5" -> 21.5,
// "true" -> true, "heat" -> "heat". False if it does not fit `cap`.
bool command_value_json(const char* payload, size_t len, char* out, size_t cap);

// A device state value (JSON scalar) as the MQTT payload Home Assistant
// matches against: true/false -> 1/0, numbers as written, strings unquoted.
bool state_payload(const char* value_json, char* out, size_t cap);

// A float stored x100 (the shadow's VAL_FLOAT) as JSON number text with at
// most two decimals: 2137 -> "21.37", 4820 -> "48.2", 2100 -> "21",
// -5 -> "-0.05". Returns length, or -1 if it does not fit.
int x100_to_json(char* out, size_t cap, int32_t v);

}  // namespace ha
