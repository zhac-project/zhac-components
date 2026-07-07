# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host harness for hap_protocol. The codec is pure C++ (only <cstdint>/
# <cstddef>/<cstring>), so it builds natively — no stubs, no ESP-IDF:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(hap_protocol_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(hap_protocol_host_test
    test_hap_protocol.cpp
    ${COMPONENTS_DIR}/hap_protocol/hap_protocol.cpp
)

target_include_directories(hap_protocol_host_test PRIVATE
    ${COMPONENTS_DIR}/hap_protocol/include
)

target_compile_options(hap_protocol_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME hap_protocol_host COMMAND hap_protocol_host_test)
