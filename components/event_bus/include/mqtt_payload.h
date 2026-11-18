// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_payload -- the whole text of an MQTT_MSG payload too long for the
// event's 32-byte field (research 2026-11-18 A12). The receiver keeps it here;
// the event keeps its first 27 characters, a NUL and the text's id (bytes
// 28-31); a consumer that wants it all (Lua's on_mqtt) asks by that id. 16
// messages of up to 1 KB, in PSRAM from the first long one on; a consumer
// that comes after 16 newer long messages gets the 27 characters.
#pragma once

#include <cstddef>
#include <cstdint>

#include "event_bus.h"

inline constexpr size_t kMqttPayloadMax = 1024;   // a longer message: its first 1 KB

// Fill m.payload from a received message: whole when it fits (31 bytes),
// else its start and the id of the whole text kept here.
void mqtt_msg_set_payload(MqttMsgEvent& m, const char* data, size_t len);

// The whole payload into `out` (a cap of kMqttPayloadMax takes any): its
// length, or 0 when m.payload holds it all or the text is gone -- m.payload
// is the answer then.
size_t mqtt_msg_full_payload(const MqttMsgEvent& m, char* out, size_t cap);
