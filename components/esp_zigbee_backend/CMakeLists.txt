# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# esp_zigbee_backend — ESP-IDF component registration.
#
# P0 is the PURE seam core only (RX mapper + TX builder); it has no
# esp-zigbee-lib dependency and no `idf_component.yml`, so a firmware that does
# not REQUIRE this component neither links it nor changes its dependencies.lock.
#
# P1 adds the `esp_zigbee_glue` (esp_zb_init/start + DeviceBackend/zhc_adapter
# registration) with its esp-zigbee-lib dependency Kconfig-gated on
# CONFIG_ZHAC_ZIGBEE_BACKEND_ESP_ZIGBEE (see design doc §4).
idf_component_register(
    SRCS "esp_zigbee_backend.cpp"
    INCLUDE_DIRS "include"
)
