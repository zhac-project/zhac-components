# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host contract suite for device_cmd: every transport calls these functions,
# so this is where attribute values, rename validation, the permit-join clamp
# and the soft/hard remove sweeps are pinned once.
cmake_minimum_required(VERSION 3.16)
project(device_cmd_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DEVICE_CMD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(COMPONENTS_DIR ${DEVICE_CMD_DIR}/..)

add_executable(device_cmd_host_test
    test_device_cmd.cpp
    stubs/host_stubs.cpp
    ${DEVICE_CMD_DIR}/device_cmd.cpp
    ${DEVICE_CMD_DIR}/device_cmd_ops.cpp
    ${DEVICE_CMD_DIR}/device_cmd_json.cpp
)
target_include_directories(device_cmd_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs                       # test_stubs.h
    ${COMPONENTS_DIR}/simple_rules/test/host/stubs          # esp_log.h / freertos shims
    ${DEVICE_CMD_DIR}/include
    ${COMPONENTS_DIR}/zap_common/include
    ${COMPONENTS_DIR}/zigbee_mgr/include
    ${COMPONENTS_DIR}/device_shadow/include
    ${COMPONENTS_DIR}/zhc_adapter/include
    ${COMPONENTS_DIR}/zap_store/include
    ${COMPONENTS_DIR}/device_backend/include
    ${COMPONENTS_DIR}/arduinojson/src
)
target_compile_options(device_cmd_host_test PRIVATE -Wall -Wextra -Werror)
enable_testing()
add_test(NAME device_cmd_host COMMAND device_cmd_host_test)
