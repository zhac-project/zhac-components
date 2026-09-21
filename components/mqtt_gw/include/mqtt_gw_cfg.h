// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_gw_cfg — the local MQTT client's settings, NVS-backed (namespace
// "mqtt_cfg": enabled, broker_url, root_topic, client_id). One implementation
// for every build that runs the client itself (wired, mono); the dual-chip S3
// keeps its own settings handler with the RainMaker uplink logic.
//
// Before this file the single-chip build's setters did not persist and
// nothing loaded the settings at boot, so a broker configured in Settings
// was gone after the next reboot.
#pragma once
#include "ArduinoJson.h"

// After mqtt_gw_init() / mqtt_gw_start(): load the stored settings, set the
// root topic and client id, and arm the client when enabled. It connects on
// mqtt_gw_on_sta_up(), which the network glue calls when the interface has
// an address.
void mqtt_gw_cfg_boot(void);

// Settings write: broker_url, mqtt_root_topic, mqtt_client_id, mqtt_enabled.
// Persists what is present and applies it live (a disabled client gets the
// new URL from NVS when enabled).
void mqtt_gw_cfg_apply(JsonDocument& doc);

// Status / settings read: mqtt_enabled, mqtt_broker (user:pass stripped),
// mqtt_client_id.
void mqtt_gw_cfg_fill_status(JsonObject d);
