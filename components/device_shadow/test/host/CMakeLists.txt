# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side characterization harness for the device_shadow component. Drives the
# REAL device_shadow.cpp + shadow_pipeline.cpp end-to-end against the proven
# zap_store host shims (in-memory NVS + esp/FreeRTOS no-ops), plus the REAL
# event_bus + zap_store (both host-buildable). Same shape as
# components/zap_store/test/host and components/rule_store/test/host.
#
# The FreeRTOS timer service task and device_shadow's own task_shadow sweep do
# not run on the host (xTaskCreatePinnedToCore is a no-op), so the tests cover
# the store / config / attr / pipeline surface and the synchronous NVS paths;
# timer-firing and the deferred sweep write are documented as not host-testable.
#
# Build:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(device_shadow_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_executable(device_shadow_host_test
    test_device_shadow.cpp
    stubs/nvs_stub.cpp
    stubs/nvs_u8_stub.cpp
    ${COMPONENTS_DIR}/device_shadow/device_shadow.cpp
    ${COMPONENTS_DIR}/device_shadow/shadow_pipeline.cpp
    ${COMPONENTS_DIR}/event_bus/event_bus.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store_flush.cpp
)

target_include_directories(device_shadow_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${COMPONENTS_DIR}/device_shadow/include
    ${COMPONENTS_DIR}/event_bus/include
    ${COMPONENTS_DIR}/zap_common/include
    ${COMPONENTS_DIR}/zap_store/include
)

target_compile_options(device_shadow_host_test PRIVATE -Wall -Wextra -Werror)

# The linked-in PRODUCTION translation units carry patterns the ESP-IDF
# component build tolerates (it does not enable -Wextra) but host g++ -Wextra
# flags — notably the mandatory-but-unused FreeRTOS task-callback parameter
# `void* arg` in task_shadow(). We must NOT modify production code to satisfy a
# host-only lint, so -Wunused-parameter is relaxed for those TUs only. The test
# file + stubs remain under the full -Wall -Wextra -Werror above.
set_source_files_properties(
    ${COMPONENTS_DIR}/device_shadow/device_shadow.cpp
    ${COMPONENTS_DIR}/device_shadow/shadow_pipeline.cpp
    ${COMPONENTS_DIR}/event_bus/event_bus.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store_flush.cpp
    TARGET_DIRECTORY device_shadow_host_test
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter"
)

enable_testing()
add_test(NAME device_shadow_host COMMAND device_shadow_host_test)
