# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# zap_tls: how the hub's TLS clients judge a server (zap_tls.h). The pure rules are in
# zap_common/include/zap_tls_policy.h, host-tested there.
idf_component_register(
    SRCS "zap_tls.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls zap_common
)
