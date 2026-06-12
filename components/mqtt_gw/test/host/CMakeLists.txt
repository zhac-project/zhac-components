# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for mqtt_gw_s3.cpp's PURE security helpers (T19):
#   - mqtt_topic_ok    (F3-T19): reject wildcard / control / overlong PUBLISH
#                                topic names.
#   - redact_userinfo  (F4-T19): strip mqtt://user:pass@host credentials
#                                before logging.
#
# mqtt_gw_s3.cpp itself links esp-mqtt + esp_crt_bundle + FreeRTOS + esp_timer
# + metrics and cannot build on the host, and a whole-TU #include would need
# a large stub surface (the entire esp_mqtt_client_* API, esp_event, the
# timer/queue/semaphore/task primitives). So — following the zap_store
# test/host precedent — these tests MIRROR the two helpers' EXACT logic
# (each clearly marked "mirror of mqtt_gw_s3.cpp:<fn> — keep in sync") and
# pin them as a security regression. If the source drifts from what is
# asserted here, this test is the canary.
#
# Build with plain cmake (no IDF):
#   export PATH=/usr/bin:$PATH
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(mqtt_gw_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mqtt_gw_helpers_host_test
    test_mqtt_gw_helpers.cpp
)

target_compile_options(mqtt_gw_helpers_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME mqtt_gw_helpers_host COMMAND mqtt_gw_helpers_host_test)
