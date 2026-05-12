# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "zigbee_mgr.cpp" "zigbee_interview.cpp" "zigbee_interview_utils.cpp" "zigbee_identity.cpp" "zigbee_configure_queue.cpp" "zcl_commands.cpp" "zcl_seq.cpp" "zhc_send_bridge.cpp" "zhc_shadow_bridge.cpp" "zhc_configure_bridge.cpp" "zigbee_pool.cpp" "zigbee_diagnostics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos zap_common zap_store event_bus znp_driver device_shadow esp_timer zhc_adapter
)
