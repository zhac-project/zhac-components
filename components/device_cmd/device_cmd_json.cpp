// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "device_cmd_json.h"

bool device_cmd_value_from_json(JsonVariantConst v, DevCmdValue* out) {
    if (!out) return false;
    if (v.is<bool>())        { *out = device_cmd_bool(v.as<bool>()); return true; }
    if (v.is<const char*>()) { *out = device_cmd_str(v.as<const char*>()); return true; }
    if (v.is<long long>() || v.is<unsigned long long>()) { *out = device_cmd_int(v.as<long long>()); return true; }
    if (v.is<float>())       { *out = device_cmd_number(v.as<double>()); return true; }
    return false;
}
