# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for the ESP-free parts of zhc_adapter. Plain cmake, no IDF:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(zhc_adapter_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_executable(wake_queue_host_test
    test_wake_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/wake_queue.cpp
)
target_include_directories(wake_queue_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_options(wake_queue_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wake_queue COMMAND wake_queue_host_test)

add_executable(meter_poll_host_test
    test_meter_poll.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/meter_poll.cpp
)
target_include_directories(meter_poll_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_options(meter_poll_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME meter_poll COMMAND meter_poll_host_test)

add_executable(def_memo_host_test test_def_memo.cpp)
target_include_directories(def_memo_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_options(def_memo_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME def_memo COMMAND def_memo_host_test)
