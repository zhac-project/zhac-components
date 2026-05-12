# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "device_shadow.cpp" "shadow_pipeline.cpp"
    INCLUDE_DIRS "include"
    REQUIRES zap_common zap_store event_bus freertos nvs_flash
    PRIV_REQUIRES metrics
)
