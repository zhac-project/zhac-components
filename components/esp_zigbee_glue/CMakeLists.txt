# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# esp_zigbee_glue — ESP-IDF component. The esp-zigbee-lib dependency is pulled
# by the config-gated idf_component.yml (only when the esp-zigbee backend is
# selected). REQUIRES are likewise conditional, so a ZNP/EZSP-default build
# compiles only the no-op path and links nothing extra (design doc §4/§11).
#
# Initialise the list before appending so it can't inherit an enclosing
# component's scope (IDF v2 recursive-evaluation caveat).
set(priv_reqs)
if(CONFIG_ZHAC_ZIGBEE_BACKEND_ESP_ZIGBEE)
    list(APPEND priv_reqs
        esp_zigbee_backend   # P0 pure mapper/builder
        zhc_adapter
        zigbee_mgr
        device_backend
        device_shadow
        esp_log)
    # espressif/esp-zigbee-lib is added automatically from idf_component.yml
    # when the backend is selected — do not list it here.
endif()

idf_component_register(
    SRCS "esp_zigbee_glue.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_reqs}
)
