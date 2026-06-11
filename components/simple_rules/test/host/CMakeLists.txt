# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for simple_rules — builds the real engine +
# event_bus against single-threaded FreeRTOS/ESP shims (see stubs/).
# Separate from test/ (IDF unity app); build with plain cmake:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(simple_rules_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(simple_rules_event_loop_test
    test_event_loop.cpp
    stubs/host_stubs.cpp
    ${COMPONENTS_DIR}/simple_rules/simple_rules.cpp
    ${COMPONENTS_DIR}/simple_rules/dsl_parser.cpp
    ${COMPONENTS_DIR}/event_bus/event_bus.cpp
)

target_include_directories(simple_rules_event_loop_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${COMPONENTS_DIR}/simple_rules/include
    ${COMPONENTS_DIR}/event_bus/include
    ${COMPONENTS_DIR}/rule_store/include
    ${COMPONENTS_DIR}/zap_common/include
    ${COMPONENTS_DIR}/zigbee_mgr/include
    ${COMPONENTS_DIR}/device_shadow/include
    ${COMPONENTS_DIR}/zhc_adapter/include
    ${COMPONENTS_DIR}/mqtt_gw/include
    ${COMPONENTS_DIR}/cron_parser/include
)

target_compile_options(simple_rules_event_loop_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME simple_rules_event_loop COMMAND simple_rules_event_loop_test)
