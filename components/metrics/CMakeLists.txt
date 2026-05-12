# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS "src/metrics.cpp"
         "src/metrics_export_prometheus.cpp"
         "src/metrics_export_mqtt.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES freertos esp_timer
)
