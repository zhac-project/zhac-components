# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "..")
set(TEST_COMPONENTS "simple_rules" CACHE STRING "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(simple_rules_test)
