# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side characterization harness for the zigbee_mgr component — the
# largest component in zhac-components (Zigbee coordinator manager + ZCL
# command builder + device pool + interview state machine). The under-test
# sources run on the host against a MOCKED ZNP transport (stubs/znp_stub.cpp
# implements znp_driver.h + znp_confirm.h at the structured MtFrame level;
# znp_driver.cpp is deliberately NOT compiled). This is the seam that makes
# host-testing possible: the mock records exactly the bytes zigbee_mgr would
# send to the CC2652 radio and injects AREQ indications back to drive the
# state machine.
#
# Real (linked, not mocked) translation units:
#   • all 12 zigbee_mgr .cpp (manager, interview, zcl_commands, pool, …)
#   • event_bus, zap_store (+flush), device_shadow (+pipeline) — all
#     host-buildable, reusing the proven device_shadow harness shims.
#
# Reused wholesale from components/device_shadow/test/host/stubs: the in-memory
# NVS (nvs_stub.cpp/nvs_u8_stub.cpp) and the esp_* / FreeRTOS no-op headers.
# task_stacks.h is taken from the REAL zap_common header (it is host-clean and
# carries the kZb* stack constants). New for this harness: znp_stub.*,
# zhc_adapter_stub.cpp, esp_random.h, and three FreeRTOS symbols zigbee_mgr
# needs beyond device_shadow (xSemaphoreCreateBinary / xTimerStart /
# uxQueueMessagesWaiting).
#
# NOT host-testable (documented, worker-task driven — xTaskCreate is a no-op so
# the interview/configure/zcl worker tasks never run): the deferred interview
# read ladder, the configure-queue drain/apply (DONE-dedup + backoff), and the
# zcl_attr decode task. We characterize everything that is synchronous: the
# pool, every ZCL command builder's recorded frame, the init SREQ ladder, and
# the AREQ-driven join/leave/rejoin/crash transitions.
#
# Characterization only — asserts the component's ACTUAL behavior; it never
# modifies production code. Build (in /tmp, never in the repo tree):
#   cmake -B /tmp/zigbee_mgr_host -S . && cmake --build /tmp/zigbee_mgr_host \
#     && ctest --test-dir /tmp/zigbee_mgr_host
cmake_minimum_required(VERSION 3.16)
project(zigbee_mgr_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMPONENTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

set(ZIGBEE_MGR_SRCS
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_mgr.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_interview.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_interview_utils.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_identity.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_configure_queue.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zcl_commands.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zcl_seq.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zhc_send_bridge.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zhc_shadow_bridge.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zhc_configure_bridge.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_pool.cpp
    ${COMPONENTS_DIR}/zigbee_mgr/zigbee_diagnostics.cpp
)

set(SUPPORT_SRCS
    ${COMPONENTS_DIR}/event_bus/event_bus.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store.cpp
    ${COMPONENTS_DIR}/zap_store/zap_store_flush.cpp
    ${COMPONENTS_DIR}/device_shadow/device_shadow.cpp
    ${COMPONENTS_DIR}/device_shadow/shadow_pipeline.cpp
)

add_executable(zigbee_mgr_host_test
    test_zigbee_mgr.cpp
    stubs/znp_stub.cpp
    stubs/zhc_adapter_stub.cpp
    stubs/nvs_stub.cpp
    stubs/nvs_u8_stub.cpp
    ${ZIGBEE_MGR_SRCS}
    ${SUPPORT_SRCS}
)

target_include_directories(zigbee_mgr_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs         # esp_*/freertos/nvs/esp_random shims — win first
    ${COMPONENTS_DIR}/zigbee_mgr/include
    ${COMPONENTS_DIR}/znp_driver/include      # REAL znp_driver.h / znp_confirm.h (impl mocked)
    ${COMPONENTS_DIR}/zhc_adapter/include      # REAL zhc_adapter.h (impl stubbed)
    ${COMPONENTS_DIR}/device_shadow/include
    ${COMPONENTS_DIR}/event_bus/include
    ${COMPONENTS_DIR}/zap_store/include
    ${COMPONENTS_DIR}/zap_common/include        # REAL task_stacks.h (kZb*), zap_common.h, zcl_attribute.h
)

target_compile_options(zigbee_mgr_host_test PRIVATE -Wall -Wextra -Werror)

# The linked-in PRODUCTION translation units carry patterns the ESP-IDF
# component build tolerates (it does not enable -Wextra) but host g++ -Wextra
# flags. We must NOT modify production code to satisfy a host-only lint, so a
# minimal set of relaxations is scoped to those TUs ONLY (the test file + stubs
# stay under the full -Wall -Wextra -Werror above). Each is a host-build
# artifact, not a real code smell:
#   -Wno-unused-parameter          mandatory-but-unused FreeRTOS task / AREQ
#                                  callback params (`void* arg`, `const MtFrame&`).
#   -Wno-unused-variable /         variables computed only to feed ESP_LOGx() —
#   -Wno-unused-but-set-variable   those macros are no-ops in the host esp_log
#                                  shim, so the values look unused here.
#   -include cstddef               zigbee_mgr.h uses bare `size_t`; the ESP-IDF
#                                  toolchain exposes ::size_t via <cstdint>,
#                                  host libstdc++ does not — force-include it
#                                  rather than edit the header.
set_source_files_properties(
    ${ZIGBEE_MGR_SRCS}
    ${SUPPORT_SRCS}
    TARGET_DIRECTORY zigbee_mgr_host_test
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-but-set-variable;-include;cstddef"
)

enable_testing()
add_test(NAME zigbee_mgr_host COMMAND zigbee_mgr_host_test)
