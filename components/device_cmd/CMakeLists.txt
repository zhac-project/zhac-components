# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# device_cmd -- the one attribute-set path. Every transport (REST, WebSocket,
# MQTT / Home Assistant, HAP from the S3, Lua, rules) validates its envelope
# and calls device_cmd_set_attr(); nothing else talks to the adapter for a
# write or mirrors a command into the shadow.
idf_component_register(
    SRCS "device_cmd.cpp" "device_cmd_ops.cpp" "device_cmd_json.cpp"
    INCLUDE_DIRS "include"
    REQUIRES zap_common zap_store zigbee_mgr zhc_adapter device_shadow device_backend event_bus arduinojson esp_timer
)
