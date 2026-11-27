# SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(remote_client_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# Backoff test uses only the pure header and cpp
add_executable(remote_backoff_tests
    remote_backoff_test.cpp
    ../remote_backoff.cpp
)
target_include_directories(remote_backoff_tests PRIVATE ../include)
add_test(NAME remote_backoff COMMAND remote_backoff_tests)

# State machine test
add_executable(remote_state_tests
    remote_state_test.cpp
    ../remote_state.cpp
)
target_include_directories(remote_state_tests PRIVATE ../include)
add_test(NAME remote_state COMMAND remote_state_tests)

# Allow-list test
add_executable(remote_allow_tests
    remote_allow_test.cpp
    ../remote_allow.cpp
)
target_include_directories(remote_allow_tests PRIVATE ../include)
add_test(NAME remote_allow COMMAND remote_allow_tests)

# NVS layer test — stub provides in-memory kv_* impl
add_executable(remote_nvs_tests
    remote_nvs_test.cpp
    remote_nvs_stub.cpp
    ../remote_nvs.cpp
)
target_include_directories(remote_nvs_tests PRIVATE ../include)
target_compile_definitions(remote_nvs_tests PRIVATE REMOTE_NVS_HOST_TEST)
add_test(NAME remote_nvs COMMAND remote_nvs_tests)

# utf8_seq.h (the one strict UTF-8 table and the send guard) lives in ws_server: the lowest component both
# send paths (the cloud link here, the hub page there) can include.
set(WS_SERVER_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../ws_server/include)

# Staging slot + page cutter (remote script editing)
find_package(Threads REQUIRED)
add_executable(remote_xfer_tests
    remote_xfer_test.cpp
    ../remote_xfer.cpp
)
target_include_directories(remote_xfer_tests PRIVATE ../include ${WS_SERVER_INC})
target_link_libraries(remote_xfer_tests PRIVATE Threads::Threads)
add_test(NAME remote_xfer COMMAND remote_xfer_tests)

# The UTF-8 guard at the send choke points (publish_event, send_reply, ws_server's frame writer)
add_executable(utf8_guard_tests utf8_guard_test.cpp)
target_include_directories(utf8_guard_tests PRIVATE ../include ${WS_SERVER_INC})
add_test(NAME utf8_guard COMMAND utf8_guard_tests)

# Whole cloud frames only, nothing after the JSON value (review 2026-11-26 HL-01); the parse runs on the
# header-only ArduinoJson, as in handle_rx_frame.
add_executable(remote_frame_tests
    remote_frame_test.cpp
    ../remote_frame.cpp
)
target_include_directories(remote_frame_tests PRIVATE ../include ${CMAKE_CURRENT_SOURCE_DIR}/../../arduinojson/src)
add_test(NAME remote_frame COMMAND remote_frame_tests)
