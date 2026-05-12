# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "test_znp_mt.cpp"
         "test_znp_confirm.cpp"
    REQUIRES znp_driver unity
)
