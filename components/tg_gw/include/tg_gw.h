// components/tg_gw/include/tg_gw.h
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void tg_gw_init(void);

// All three return true if accepted-by-S3 (queued for HTTPS); false if
// input invalid or HAP send failed. Fire-and-forget — actual HTTPS
// outcome is logged on S3.
bool tg_gw_settoken(const char* token);
bool tg_gw_setchat(const char* chat_id_str);
bool tg_gw_send(const char* text,
                const char* chat_id_or_null,
                const char* parse_mode_or_null);

#ifdef __cplusplus
}
#endif
