# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "rule_store.cpp" "rule_store_flush.cpp"
    INCLUDE_DIRS "include"
    REQUIRES zap_common nvs_flash esp_rom esp_timer freertos
)
