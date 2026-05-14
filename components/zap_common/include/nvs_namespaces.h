// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Central registry of every NVS namespace string used across both
// firmware images. X-F3 in docs/OPTIMIZATIONS.md flagged that 10
// namespaces lived as scattered string literals (`"zap_v0"`,
// `"sys_cfg"`, `"mqtt_cfg"`, …) with no single place to audit them on
// upgrade migrations. Keep this file as the source of truth — every
// `nvs_open()` call site should reference one of these constants
// rather than inlining the literal.
//
// Schema versioning: each namespace owns its own scheme. Where the
// component bumps a schema-version byte at boot (only `device_shadow`
// today) the version constant lives next to the namespace below and
// the component MUST read+compare on `nvs_open` and erase the blob
// when it diverges. New namespaces should adopt the same pattern.
//
// Namespace names are limited by ESP-IDF to 15 characters. Keep them
// stable — renaming a namespace in NVS is a destructive migration.
#pragma once

#include <cstdint>

namespace zap_nvs {

// ── Zigbee / device pool ────────────────────────────────────────────
// Devices, groups, joined-network metadata. The trailing version
// suffix (`_v0`) was the v0→v1 migration gate; subsequent layout
// bumps are tracked via the schema_version byte inside each blob.
inline constexpr const char* DEVICE_POOL    = "zap_v0";

// Rule slots (`RuleSlot` × 256 max), CRC32-protected. Layout v2 since
// the `name[24]` field landed (RuleSlot = 536 B).
inline constexpr const char* RULES          = "zap_rules";

// Per-device shadow attribute cache + per-device DeviceConfig.
// Version byte stored under key "ver"; current schema is v6
// (`ATTR_KEY_MAX=28`, `ATTR_STR_MAX=48`, `ShadowAttr=84 B`).
inline constexpr const char* SHADOW         = "zap_shadow";
inline constexpr uint8_t     SHADOW_VERSION = 6;

// Zigbee coordinator config (channel, network key, panid). Wiped on
// `zigbee_factory_reset` from `firmware/p4_core/main/main.cpp`.
inline constexpr const char* ZIGBEE_CFG     = "zigbee_cfg";

// ── S3 system / network ─────────────────────────────────────────────
// Misc system flags + the API token. Auth-token rotation goes
// through `auth_rotate_token()` in `firmware/s3_core/main/main.cpp`
// (CC-F8 in docs/FINDINGS.md).
inline constexpr const char* SYS_CFG        = "sys_cfg";

// API auth state (token, enabled flag). Per-namespace separation
// keeps the rotation path narrow.
inline constexpr const char* AUTH           = "zhac_auth";

// MQTT broker URL, client ID, root topic, last-will config. Read by
// both the S3 mqtt_gw and the P4 forwarder.
inline constexpr const char* MQTT_CFG       = "mqtt_cfg";

// WiFi STA + AP credentials, captive-portal disable flag, last
// known SSID. Plaintext today — encryption is a TODO
// (CC-F8 in docs/FINDINGS.md).
inline constexpr const char* WIFI_CFG       = "wifi_cfg";

// Log sinks: WS-streaming on/off + level, MQTT-streaming on/off +
// level. Default off after the contention-bug fix described in
// `firmware/s3_core/main/log_ring.cpp`.
inline constexpr const char* LOG_CFG        = "log_cfg";

// ── Misc / per-feature opt-in ───────────────────────────────────────
// Feature opt-in toggles ("zigbee permit-join always on", etc.)
// Surfaced through the SPA Settings page.
inline constexpr const char* OPTIONS        = "zhac_opt";

}  // namespace zap_nvs

// C-callers: same string constants exposed via macros. Headers like
// `device_shadow.cpp` that are C++ but want a `static const char*`
// can do `static const char* NVS_NS = ZAP_NVS_SHADOW;` to migrate
// without renaming locals.
#define ZAP_NVS_DEVICE_POOL "zap_v0"
#define ZAP_NVS_RULES       "zap_rules"
#define ZAP_NVS_SHADOW      "zap_shadow"
#define ZAP_NVS_ZIGBEE_CFG  "zigbee_cfg"
#define ZAP_NVS_SYS_CFG     "sys_cfg"
#define ZAP_NVS_AUTH        "zhac_auth"
#define ZAP_NVS_MQTT_CFG    "mqtt_cfg"
#define ZAP_NVS_WIFI_CFG    "wifi_cfg"
#define ZAP_NVS_LOG_CFG     "log_cfg"
#define ZAP_NVS_OPTIONS     "zhac_opt"
