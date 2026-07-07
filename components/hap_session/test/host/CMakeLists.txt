# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side characterization harness for hap_session — the HAP SPI reliability
# layer (sliding window, retransmit, ACK echo, receive-side dedup, SYNC). Builds
# the real hap_session.cpp against minimal esp/FreeRTOS shims + a controllable
# virtual clock (stubs/). hap_protocol.h is header-only for our purposes
# (HapFrame + the inline hap_make_reply), so hap_protocol.cpp is NOT compiled.
# No ESP-IDF needed:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(hap_session_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(hap_session_host_test
    test_hap_session.cpp
    stubs/hap_session_stubs.cpp
    ${COMPONENTS_DIR}/hap_session/hap_session.cpp
)

target_include_directories(hap_session_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${COMPONENTS_DIR}/hap_session/include
    ${COMPONENTS_DIR}/hap_protocol/include
)

target_compile_options(hap_session_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME hap_session_host COMMAND hap_session_host_test)
