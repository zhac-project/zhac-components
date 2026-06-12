// components/tg_gw/include/tg_gw.h
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

// F8-T19: single source of truth for the max accepted Telegram bot-token
// length, shared by BOTH cores. Real tokens are `<bot_id>:<35-char-secret>`
// ≈ 46 chars; 80 gives generous headroom while staying within the 96-byte
// HapTgSettoken.token[97] HAP field (so a SET never silently truncates).
// Previously S3 capped at 80 and P4 at 96 → an 81–96-char token was
// accepted P4-side and silently dropped on S3.
#define TG_TOKEN_MAX 80

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
