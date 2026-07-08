# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side characterization harness for the znp_driver transport. Drives the
# REAL host-tractable TUs — the MT wire codec (znp_parser.cpp), AREQ dispatch
# (znp_areq_dispatch.cpp), the AF_DATA_CONFIRM ring (znp_confirm.cpp), and the
# state/stats support TU it links against (znp_state.cpp) — against thin
# esp/FreeRTOS/UART host shims. The FreeRTOS semaphore shim implements REAL
# 0/1 counting semantics because the confirm ring drains its slot semaphore on
# reserve (an always-pdTRUE stub would spin forever there).
#
# Out of scope (same shape as device_shadow/test/host): the SREQ/SRSP worker
# round-trip and the RX/UART pump (znp_transport.cpp / znp_rx.cpp /
# znp_worker.cpp). Those need the RX worker task + UART event queue, and
# xTaskCreate is a no-op on the host, so the async pump never runs.
#
# Build:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(znp_driver_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC_DIR ${COMPONENT_DIR}/src)

add_executable(znp_driver_host_test
    test_znp_driver.cpp
    stubs/host_stubs.cpp
    ${SRC_DIR}/znp_parser.cpp
    ${SRC_DIR}/znp_areq_dispatch.cpp
    ${SRC_DIR}/znp_confirm.cpp
    ${SRC_DIR}/znp_state.cpp
)

target_include_directories(znp_driver_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs   # esp/FreeRTOS/driver shims (win over IDF)
    ${COMPONENT_DIR}/include            # real public headers
    ${SRC_DIR}                          # real private znp_internal.h
)

target_compile_options(znp_driver_host_test PRIVATE -Wall -Wextra -Werror)

# The linked-in PRODUCTION translation units carry patterns the ESP-IDF
# component build tolerates (it does not enable -Wextra) but host g++ -Wextra
# flags. We must NOT modify production code to satisfy a host-only lint, so the
# relevant warnings are relaxed for those TUs only; the test file + stubs stay
# under the full -Wall -Wextra -Werror above. Specifically:
#   -Wno-sign-compare    : the MT parser's `len_ > ZNP_MAX_DATA_LEN` guard
#                          (uint8_t vs size_t).
#   -Wno-unused-function : znp_state.cpp's name_of() is referenced only inside
#                          ESP_LOGI(), which the host esp_log shim expands to a
#                          no-op — so it looks unused here though the real IDF
#                          build uses it.
set_source_files_properties(
    ${SRC_DIR}/znp_parser.cpp
    ${SRC_DIR}/znp_areq_dispatch.cpp
    ${SRC_DIR}/znp_confirm.cpp
    ${SRC_DIR}/znp_state.cpp
    TARGET_DIRECTORY znp_driver_host_test
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-function"
)

enable_testing()
add_test(NAME znp_driver_host COMMAND znp_driver_host_test)
