# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for rule_store — builds the real rule_store.cpp +
# rule_store_flush.cpp against an in-memory NVS stub + minimal esp/FreeRTOS
# shims (stubs/). No ESP-IDF needed:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(rule_store_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(rule_store_host_test
    test_rule_store.cpp
    stubs/nvs_stub.cpp
    ${COMPONENTS_DIR}/rule_store/rule_store.cpp
    ${COMPONENTS_DIR}/rule_store/rule_store_flush.cpp
)

target_include_directories(rule_store_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${COMPONENTS_DIR}/rule_store/include
    ${COMPONENTS_DIR}/zap_common/include
)

target_compile_options(rule_store_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME rule_store_host COMMAND rule_store_host_test)
