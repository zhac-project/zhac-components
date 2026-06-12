# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for hap_json's device-list encoder (HOTFIX:
# device-list paging). Compiles the REAL hap_json.cpp against the real
# ArduinoJson + zap_common/hap_protocol headers, with a local esp_log.h
# stub (counting ESP_LOGE) — NO IDF, NO SPI, NO FreeRTOS needed because
# encode_device_list is pure JSON serialisation.
#
# Build:
#   export PATH=/usr/bin:$PATH      # keep ESP32-ULP `as` off the PATH
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(hap_json_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Component roots (this file lives at components/hap_json/test/host/).
set(HAP_JSON_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(COMPONENTS_DIR ${HAP_JSON_DIR}/..)

add_executable(hap_devlist_host_test
    test_devlist_paging.cpp
    ${HAP_JSON_DIR}/hap_json.cpp
)

target_include_directories(hap_devlist_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs                       # esp_log.h shim
    ${HAP_JSON_DIR}/include                                 # hap_json.h
    ${COMPONENTS_DIR}/zap_common/include                    # zap_common.h, zcl_attribute.h
    ${COMPONENTS_DIR}/hap_protocol/include                  # hap_protocol.h
    ${COMPONENTS_DIR}/arduinojson/src                       # ArduinoJson.h
)

# Same strictness the rest of the tree builds under.
target_compile_options(hap_devlist_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME hap_devlist_host COMMAND hap_devlist_host_test)
