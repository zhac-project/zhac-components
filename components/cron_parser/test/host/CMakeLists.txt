# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for cron_parser. cron_parser is pure C++ (only
# <ctime>/<cstdlib>, no FreeRTOS / ESP-IDF), so it builds and runs natively
# with plain cmake — no stubs needed:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(cron_parser_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(cron_parser_host_test
    test_cron.cpp
    ${COMPONENTS_DIR}/cron_parser/cron_parser.cpp
)

target_include_directories(cron_parser_host_test PRIVATE
    ${COMPONENTS_DIR}/cron_parser/include
)

target_compile_options(cron_parser_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME cron_parser_host COMMAND cron_parser_host_test)
