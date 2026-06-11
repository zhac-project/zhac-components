# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for event_bus — builds the real event_bus.cpp
# against the single-threaded FreeRTOS/ESP shims shared with the
# simple_rules host harness (../../../simple_rules/test/host/stubs — single
# source of truth for the shims; keep both suites green when touching them).
# Separate from any IDF unity app; build with plain cmake:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(event_bus_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SHARED_STUBS_DIR ${COMPONENTS_DIR}/simple_rules/test/host/stubs)

add_executable(event_bus_host_test
    test_event_bus.cpp
    ${SHARED_STUBS_DIR}/host_stubs.cpp
    ${COMPONENTS_DIR}/event_bus/event_bus.cpp
)

target_include_directories(event_bus_host_test PRIVATE
    ${SHARED_STUBS_DIR}
    ${COMPONENTS_DIR}/event_bus/include
    # below: only because the shared host_stubs.cpp stubs simple_rules'
    # link-time dependencies and includes their headers
    ${COMPONENTS_DIR}/simple_rules/include
    ${COMPONENTS_DIR}/rule_store/include
    ${COMPONENTS_DIR}/zap_common/include
    ${COMPONENTS_DIR}/zigbee_mgr/include
    ${COMPONENTS_DIR}/device_shadow/include
    ${COMPONENTS_DIR}/zhc_adapter/include
    ${COMPONENTS_DIR}/mqtt_gw/include
    ${COMPONENTS_DIR}/cron_parser/include
)

target_compile_options(event_bus_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME event_bus_host COMMAND event_bus_host_test)
