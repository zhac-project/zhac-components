# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_build_get_property(target IDF_TARGET)
# A single-chip firmware on a non-S3 target (zhac-wired-core: P4/S31) has no
# S3 to forward to, so it needs the real client too. It says so from its
# project CMakeLists, before project():
#   idf_build_set_property(ZHAC_MQTT_GW_LOCAL_CLIENT 1)
# A build property, not Kconfig: it must be readable in IDF's early
# component-expansion pass, where CONFIG_* symbols do not exist yet.
idf_build_get_property(local_client ZHAC_MQTT_GW_LOCAL_CLIENT)

if(target STREQUAL "esp32s3" OR local_client)
    set(MQTT_GW_SRCS "mqtt_gw_s3.cpp" "mqtt_gw_cfg.cpp")
    # `metrics` carries METRIC_MQTT_DROPPED_MSGS (F-07).
    # `zap_store` carries the uplink selector (zhac_uplink_get, RainMaker
    # bridge Phase 2) that gates whether this gateway is allowed to start.
    set(MQTT_GW_REQS mqtt freertos zap_common metrics zap_store nvs_flash arduinojson)
else()
    set(MQTT_GW_SRCS "mqtt_gw_p4.cpp")
    set(MQTT_GW_REQS hap_slave hap_session hap_json hap_protocol freertos)
endif()

idf_component_register(
    SRCS ${MQTT_GW_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${MQTT_GW_REQS}
)
