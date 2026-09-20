// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_bridge — Home Assistant MQTT discovery and control for ZHAC.
//
// Off by default; turned on from Settings (`ha_discovery`, `ha_prefix`),
// persisted in NVS `mqtt_cfg`. When on, and whenever the MQTT client
// (re)connects, it publishes a retained discovery config per entity (see
// ha_discovery.h for the topics), keeps a retained per-attribute state topic
// current, and turns `.../<key>/set` messages into attribute writes.
//
// Device data lives in different places per firmware — on the P4, reached
// over HAP, for the dual-chip S3; in the local pool for the single-chip
// builds — so each firmware supplies it through HaBridgePlatform. Everything
// here runs on its own task; the callbacks may block.
#pragma once
#include <cstdint>

#include "ha_discovery.h"

struct HaDeviceSnapshot {
    uint64_t    ieee;
    const char* name;
    const char* vendor;
    const char* model;
    const char* exposes;   // JSON array, as zhac_adapter_build_exposes_json emits
    const char* attrs;     // JSON object of current values ({"temperature":21.4}), or null
};
using HaDeviceCb = void (*)(const HaDeviceSnapshot& dev, void* ctx);

struct HaBridgePlatform {
    const char* hub_model;   // shown on the hub's HA device, e.g. "ESP32-P4 wired"
    void (*for_each_device)(HaDeviceCb cb, void* ctx);
    bool (*get_device)(uint64_t ieee, HaDeviceCb cb, void* ctx);
    // `value_json` is a JSON scalar: 1, 21.5, true, "heat".
    bool (*set_attr)(uint64_t ieee, const char* key, const char* value_json);
};

// Loads the NVS settings and starts the task. `platform` must outlive it.
void        ha_bridge_init(const HaBridgePlatform* platform);
bool        ha_bridge_enabled(void);
const char* ha_bridge_prefix(void);
// Persist and apply. Turning it off, or changing the prefix, first retracts
// every config it published, so Home Assistant drops the entities.
void        ha_bridge_configure(bool enabled, const char* prefix);

// Device lifecycle, from the firmware's own event paths. Non-blocking.
void ha_bridge_device_changed(uint64_t ieee);   // joined, renamed, re-interviewed
void ha_bridge_device_removed(uint64_t ieee);

// One attribute changed. `value_json` is a JSON scalar. Non-blocking; a
// no-op while discovery is off or the broker is down.
void ha_bridge_publish_state(uint64_t ieee, const char* key, const char* value_json);

// Offer an inbound MQTT message from the mqtt_gw rx callback (esp-mqtt's
// task). Returns true if it was a command for this hub; it is then queued,
// never handled inline.
bool ha_bridge_on_mqtt_rx(const char* topic, int topic_len, const char* data, int data_len);
