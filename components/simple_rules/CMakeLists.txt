# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "dsl_parser.cpp" "simple_rules.cpp" "expr_eval.cpp"
    INCLUDE_DIRS "include"
    REQUIRES zap_common rule_store event_bus zhc_adapter mqtt_gw zigbee_mgr freertos cron_parser
)
