// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for tg_gw_body.cpp: the pure part of the single-chip Telegram
// sender (token / chat checks, the sendMessage JSON body). Links the real
// file, unlike the older mirror tests next to it.
#include "tg_gw_body.h"

#include <cstdio>
#include <cstring>
#include <string>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

static std::string body(const char* text, const char* chat, const char* pm, size_t cap = 4096) {
    static char out[8192];
    const int n = tg_build_body(text, chat, pm, out, cap);
    return n < 0 ? std::string("<-1>") : std::string(out, static_cast<size_t>(n));
}

int main() {
    // Token: <digits>:<secret of [A-Za-z0-9_-]>, at most TG_TOKEN_MAX. It goes into the URL path.
    CHECK(tg_token_ok("123456789:AAH-abc_DEF"));
    CHECK(!tg_token_ok(nullptr));
    CHECK(!tg_token_ok(""));
    CHECK(!tg_token_ok("123456789"));              // no secret
    CHECK(!tg_token_ok(":abc"));                   // no bot id
    CHECK(!tg_token_ok("12a:abc"));                // bot id not digits
    CHECK(!tg_token_ok("123:ab/../x"));            // would change the URL path
    CHECK(!tg_token_ok("123:ab?x=1"));
    CHECK(!tg_token_ok("123:ab c"));
    {
        std::string t = "1:" + std::string(TG_TOKEN_MAX - 2, 'a');
        CHECK(tg_token_ok(t.c_str()));
        t += 'a';
        CHECK(!tg_token_ok(t.c_str()));
    }

    // Chat: a numeric id (groups are negative) or @channelusername.
    CHECK(tg_chat_ok("1234567890"));
    CHECK(tg_chat_ok("-1001234567890"));
    CHECK(tg_chat_ok("@my_channel"));
    CHECK(!tg_chat_ok(nullptr));
    CHECK(!tg_chat_ok(""));
    CHECK(!tg_chat_ok("-"));
    CHECK(!tg_chat_ok("12-3"));
    CHECK(!tg_chat_ok("@"));
    CHECK(!tg_chat_ok("@bad\"chat"));
    CHECK(!tg_chat_ok("12345678901234567890123456789012345678901"));   // over TG_CHAT_MAX

    // Body.
    CHECK(body("hi", "123", nullptr) == R"({"chat_id":"123","text":"hi"})");
    CHECK(body("hi", "123", "") == R"({"chat_id":"123","text":"hi"})");
    CHECK(body("<b>hi</b>", "-100", "HTML") == R"({"chat_id":"-100","text":"<b>hi</b>","parse_mode":"HTML"})");
    CHECK(body("x", "1", "Markdown") == R"({"chat_id":"1","text":"x","parse_mode":"Markdown"})");
    CHECK(body("x", "1", "MarkdownV2") == R"({"chat_id":"1","text":"x","parse_mode":"MarkdownV2"})");
    CHECK(body("x", "1", "markdown") == "<-1>");                       // whitelist is exact
    CHECK(body("x", "1", "HTML\",\"x\":\"1") == "<-1>");
    CHECK(body("a\"b\\c\nd\te\x01" "f", "1", nullptr) == R"({"chat_id":"1","text":"a\"b\\c\nd\tef"})");
    CHECK(body(nullptr, "1", nullptr) == "<-1>");
    CHECK(body("x", nullptr, nullptr) == "<-1>");
    CHECK(body("x", "12345678901234567890123456789012345678901", nullptr) == "<-1>");   // too long: refused whole
    CHECK(body("hello", "123", nullptr, 20) == "<-1>");               // does not fit: refused, not cut
    {
        // Long text is cut to TG_TEXT_ESC_MAX escaped bytes on a UTF-8 boundary; the body stays valid.
        std::string t;
        for (int i = 0; i < 2000; i++) t += "\xD0\x96";               // Ж, 2 bytes each
        const std::string b = body(t.c_str(), "1", nullptr);
        CHECK(b != "<-1>");
        const size_t a = b.find("\"text\":\"") + 8, z = b.rfind("\"}");
        CHECK(z - a <= TG_TEXT_ESC_MAX);
        CHECK((z - a) % 2 == 0);                                       // no half character
        CHECK(b.substr(b.size() - 2) == "\"}");
    }

    // A send that got no answer (no network, DNS, TLS, a timeout), 429 and 5xx are tried again;
    // any other answer is final (a bad token or chat stays bad).
    CHECK(tg_send_retry(false, 0));
    CHECK(tg_send_retry(true, 429));
    CHECK(tg_send_retry(true, 500));
    CHECK(tg_send_retry(true, 502));
    CHECK(!tg_send_retry(true, 200));
    CHECK(!tg_send_retry(true, 400));
    CHECK(!tg_send_retry(true, 401));
    CHECK(!tg_send_retry(true, 403));

    if (s_failures) { std::printf("test_tg_gw_body: %d failure(s)\n", s_failures); return 1; }
    std::printf("test_tg_gw_body: all passed\n");
    return 0;
}
