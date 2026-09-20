// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The JSON `value` of device.attr.set / PUT attrs / an MQTT command, as every
// transport used to parse it on its own: bool, integer, decimal or string. A
// string that looks like a number stays a string (an enum option's raw code
// is the converter's business). Kept apart from device_cmd.h so callers
// without ArduinoJson (Lua, the rule engine) do not pull it in.
#pragma once

#include "ArduinoJson.h"
#include "device_cmd.h"

// Returns false for null, objects and arrays (DEVCMD_BAD_VALUE territory).
bool device_cmd_value_from_json(JsonVariantConst v, DevCmdValue* out);
