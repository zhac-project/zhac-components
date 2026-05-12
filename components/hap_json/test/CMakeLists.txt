# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "..")
set(TEST_COMPONENTS "hap_json" CACHE STRING "")
idf_build_set_property(MINIMAL_BUILD ON)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hap_json_test)
