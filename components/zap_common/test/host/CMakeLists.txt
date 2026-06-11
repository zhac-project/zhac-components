# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: Apache-2.0
#
# Host-side test harness for zap_common (header-only component). Builds
# against the local stubs/ shims — NOT the shared simple_rules host
# shims: those carry no esp_err.h and this suite needs a counting
# ESP_LOGE to assert nvs_checked.h's per-op logging.
# Build with plain cmake (no IDF):
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(zap_common_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(nvs_checked_host_test
    test_nvs_checked.cpp
)

target_include_directories(nvs_checked_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_options(nvs_checked_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME nvs_checked_host COMMAND nvs_checked_host_test)
