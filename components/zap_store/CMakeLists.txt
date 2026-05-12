# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "zap_store.cpp" "zap_store_flush.cpp"
    INCLUDE_DIRS "include"
    REQUIRES spi_flash nvs_flash zap_common freertos esp_timer
)
