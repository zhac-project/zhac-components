# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for zap_store's PURE logic (FINDINGS §8 P2):
# capacity-reject decision (§8.1) + CRC in-place equivalence (§8.4).
# zap_store.cpp itself links NVS + FreeRTOS and cannot build on the host;
# these tests mirror the source predicates against the real ZapDevice
# layout (zap_common/include) and a real CRC32 (local stub). Keep the
# mirrored predicates in lockstep with zap_store.cpp.
#
# Build with plain cmake (no IDF):
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(zap_store_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(zap_store_logic_host_test
    test_zap_store_logic.cpp
)

target_include_directories(zap_store_logic_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../zap_common/include
)

target_compile_options(zap_store_logic_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME zap_store_logic_host COMMAND zap_store_logic_host_test)
