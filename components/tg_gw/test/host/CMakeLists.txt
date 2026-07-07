# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host-side test harness for tg_gw_s3.cpp's PURE security helpers (T19):
#   - json_escape  (F9-T19): escape ", \, control bytes, \n \r \t into a
#                            capped buffer; on truncation back off to the
#                            last complete UTF-8 code-point boundary so the
#                            Telegram API never sees invalid UTF-8 / broken
#                            JSON framing. The chat-id injection case
#                            (chat = 123","x":"y) MUST come out fully
#                            escaped — no unescaped '"' that breaks framing.
#   - parse_mode whitelist (F7-T19): only Markdown / MarkdownV2 / HTML are
#                            accepted; anything else is rejected.
#
# tg_gw_s3.cpp links hap_json + esp_http_client + nvs + FreeRTOS and cannot
# build on the host; a whole-TU #include would need a large stub surface.
# So — matching the zap_store/test/host precedent — these tests MIRROR the
# helpers' EXACT logic (each marked "mirror of tg_gw_s3.cpp:<...> — keep in
# sync") and pin them as a security regression.
#
# Build with plain cmake (no IDF):
#   export PATH=/usr/bin:$PATH
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(tg_gw_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(tg_gw_helpers_host_test
    test_tg_gw_helpers.cpp
)

target_compile_options(tg_gw_helpers_host_test PRIVATE -Wall -Wextra -Werror)

# Characterization MATRIX: P4 accept-gates, cross-core token cap (F8-T19),
# S3 compose (chat select / URL / body), parse_mode second gate, and the
# json_escape edges the helpers test omits. Mirrors the pure fragments of
# both TUs (neither links on the host); needs include/ for TG_TOKEN_MAX.
add_executable(tg_gw_matrix_host_test
    test_tg_gw_matrix.cpp
)

target_include_directories(tg_gw_matrix_host_test PRIVATE ../../include)
target_compile_options(tg_gw_matrix_host_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME tg_gw_helpers_host COMMAND tg_gw_helpers_host_test)
add_test(NAME tg_gw_matrix_host COMMAND tg_gw_matrix_host_test)
