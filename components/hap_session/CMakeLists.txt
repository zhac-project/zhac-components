# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "hap_session.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol freertos
    PRIV_REQUIRES metrics esp_timer
)
