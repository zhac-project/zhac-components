# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for ha_discovery.cpp, the pure half of ha_bridge. No IDF:
#   export PATH=/usr/bin:$PATH
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(ha_bridge_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(ha_discovery_host_test
    test_ha_discovery.cpp
    ${COMPONENTS_DIR}/ha_bridge/ha_discovery.cpp
)
target_include_directories(ha_discovery_host_test PRIVATE
    ${COMPONENTS_DIR}/ha_bridge/include
    ${COMPONENTS_DIR}/arduinojson/src
)
target_compile_options(ha_discovery_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME ha_discovery COMMAND ha_discovery_host_test)
