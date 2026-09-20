# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "ntp_cfg.cpp"
    INCLUDE_DIRS "include"
    REQUIRES zap_common
    PRIV_REQUIRES lwip nvs_flash esp_netif
)
