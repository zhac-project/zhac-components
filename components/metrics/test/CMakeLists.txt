# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "..")

set(TEST_COMPONENTS "metrics" CACHE STRING "List of components to test")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(metrics_test)
