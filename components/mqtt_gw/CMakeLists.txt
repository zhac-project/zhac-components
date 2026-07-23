# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "esp32s3")
    set(MQTT_GW_SRCS "mqtt_gw_s3.cpp")
    # `metrics` carries METRIC_MQTT_DROPPED_MSGS (F-07).
    # `zap_store` carries the uplink selector (zhac_uplink_get, RainMaker
    # bridge Phase 2) that gates whether this gateway is allowed to start.
    set(MQTT_GW_REQS mqtt freertos zap_common metrics zap_store)
else()
    set(MQTT_GW_SRCS "mqtt_gw_p4.cpp")
    set(MQTT_GW_REQS hap_slave hap_session hap_json hap_protocol freertos)
endif()

idf_component_register(
    SRCS ${MQTT_GW_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${MQTT_GW_REQS}
)
