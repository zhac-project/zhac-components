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
    ${COMPONENTS_DIR}/zap_common/include
)
target_compile_options(ha_discovery_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME ha_discovery COMMAND ha_discovery_host_test)

# ha_bridge_on_mqtt_rx(), cut out of ha_bridge.cpp (IDF-bound as a whole) at build time, so the test
# always runs the current code: an empty or stale slice fails to compile or to pass.
set(HA_BRIDGE_CPP ${COMPONENTS_DIR}/ha_bridge/ha_bridge.cpp)
set(RX_SLICE ${CMAKE_CURRENT_BINARY_DIR}/on_mqtt_rx.inc)
add_custom_command(
    OUTPUT ${RX_SLICE}
    COMMAND sh -c "sed -n '/^bool ha_bridge_on_mqtt_rx/,/^}/p' \"$0\" > \"$1\"" ${HA_BRIDGE_CPP} ${RX_SLICE}
    DEPENDS ${HA_BRIDGE_CPP}
    VERBATIM)
add_executable(ha_command_rx_host_test
    test_command_rx.cpp
    ${RX_SLICE}
    ${COMPONENTS_DIR}/ha_bridge/ha_discovery.cpp
)
target_include_directories(ha_command_rx_host_test PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${COMPONENTS_DIR}/ha_bridge/include
    ${COMPONENTS_DIR}/arduinojson/src
    ${COMPONENTS_DIR}/zap_common/include
)
target_compile_options(ha_command_rx_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ha_command_rx COMMAND ha_command_rx_host_test)
