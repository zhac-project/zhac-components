// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pure part of the single-chip Telegram sender (tg_gw_local.cpp):
// input checks and the sendMessage JSON body. Host-tested.
#pragma once
#include <cstddef>

#include "tg_gw.h"   // TG_TOKEN_MAX

constexpr size_t TG_CHAT_MAX     = 40;     // longest chat id or @username taken
constexpr size_t TG_TEXT_ESC_MAX = 3200;   // escaped text bytes per message; longer text is cut

// <digits>:<[A-Za-z0-9_-]+>, at most TG_TOKEN_MAX chars. It lands in the URL path, so nothing else.
bool tg_token_ok(const char* token);

// A numeric id (negative for groups) or @username, at most TG_CHAT_MAX chars.
bool tg_chat_ok(const char* chat);

// {"chat_id":"..","text":"..",["parse_mode":".."]} into out. Bytes written, or -1 when the
// chat is not valid, parse_mode is not Markdown / MarkdownV2 / HTML (null or "" = none), or
// the body does not fit. Text over TG_TEXT_ESC_MAX escaped bytes is cut on a UTF-8 boundary.
int tg_build_body(const char* text, const char* chat, const char* parse_mode, char* out, size_t cap);

// Is a failed send worth trying again? `answered` is false when the request got no HTTP answer (no
// network, DNS, TLS, a timeout); then `status` is ignored. 429 and 5xx are tried again too; any other
// answer is final (a bad token or chat stays bad).
bool tg_send_retry(bool answered, int status);
