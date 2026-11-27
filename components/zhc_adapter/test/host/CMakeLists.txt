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

add_executable(tuya_mcu_host_test test_tuya_mcu.cpp)
target_include_directories(tuya_mcu_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../zap_common/include)
target_compile_options(tuya_mcu_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME tuya_mcu COMMAND tuya_mcu_host_test)

# zhc_adapter.cpp's glue, cut out at build time against the real meter_poll.cpp: fire_shadow_updates (the shadow
# bridge's report, review 2026-11-19 P3 #1) and the availability ping's zhac_adapter_try_decode, MeterLock,
# ping_heard and ping_quiet_device (research 2026-11-18 B7, review P3 #3/#4). The DispatchResult type comes from
# embedded-zhc ($EMBEDDED_ZHC_PATH, else the sibling checkout).
set(ZHC_ADAPTER_CPP ${CMAKE_CURRENT_SOURCE_DIR}/../../src/zhc_adapter.cpp)
if(DEFINED ENV{EMBEDDED_ZHC_PATH})
    set(EZHC_INCLUDE $ENV{EMBEDDED_ZHC_PATH}/include)
else()
    set(EZHC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../embedded-zhc/include)
endif()
set(GLUE_SLICE ${CMAKE_CURRENT_BINARY_DIR}/adapter_glue.inc)
add_custom_command(
    OUTPUT ${GLUE_SLICE}
    COMMAND sh -c "sed -n -e '/^void fire_shadow_updates(/,/^}/p' -e '/^extern \"C\" bool zhac_adapter_try_decode(/,/^}/p' -e '/^struct MeterLock {/,/^};/p' -e '/^void ping_heard(uint64_t ieee) {/,/^}/p' -e '/^void ping_quiet_device(/,/^}/p' -e '/^extern \"C\" void zhac_adapter_unreachable(/,/^}/p' -e '/^extern \"C\" void zhac_adapter_heard(/,/^}/p' \"$0\" > \"$1\"" ${ZHC_ADAPTER_CPP} ${GLUE_SLICE}
    DEPENDS ${ZHC_ADAPTER_CPP}
    VERBATIM)
add_executable(adapter_glue_host_test
    test_adapter_glue.cpp
    ${GLUE_SLICE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/meter_poll.cpp
)
target_include_directories(adapter_glue_host_test PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${EZHC_INCLUDE})
target_compile_options(adapter_glue_host_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME adapter_glue COMMAND adapter_glue_host_test)
