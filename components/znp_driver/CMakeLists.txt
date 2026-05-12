# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "src/znp_parser.cpp"
         "src/znp_rx.cpp"
         "src/znp_worker.cpp"
         "src/znp_areq_dispatch.cpp"
         "src/znp_state.cpp"
         "src/znp_transport.cpp"
         "src/znp_driver_shim.cpp"
         "src/znp_confirm.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_driver_uart esp_driver_gpio freertos esp_timer zap_common
    PRIV_REQUIRES metrics
)
