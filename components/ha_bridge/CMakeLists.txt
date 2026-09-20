# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ha_bridge — Home Assistant MQTT discovery. ha_discovery.cpp is pure (host
# tests in test/host); ha_bridge.cpp is the ESP-IDF task around it. Only
# firmwares with a real MQTT client link it (net-core, wired-core).
idf_component_register(
    SRCS "ha_discovery.cpp" "ha_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_gw
    PRIV_REQUIRES arduinojson nvs_flash esp_app_format freertos
)
