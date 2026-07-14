# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for esp_zigbee_backend's pure RX mapper / TX builder
# (P0). Self-contained — no FreeRTOS/ESP shims, no esp-zigbee-lib. Build with
# plain cmake:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(esp_zigbee_backend_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(esp_zigbee_backend_host_test
    test_esp_zigbee_backend.cpp
    ${COMPONENT_DIR}/esp_zigbee_backend.cpp
)

target_include_directories(esp_zigbee_backend_host_test PRIVATE
    ${COMPONENT_DIR}/include
)

target_compile_options(esp_zigbee_backend_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME esp_zigbee_backend_host COMMAND esp_zigbee_backend_host_test)
