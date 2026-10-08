// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The UTF-8 guard at the hub's send choke points: remote_client_publish_event and remote_client_send_reply
// (the cloud link) and ws_server's frame writer (the hub page). Chrome and Tomcat close the socket on one
// invalid byte in a text frame, so each byte of an invalid sequence goes out as '?'. Structural JSON bytes
// are ASCII, so the frame stays valid JSON of the same length.
#include "utf8_seq.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static std::string scrubbed(std::string s, size_t* bad = nullptr) {
    const size_t n = utf8_scrub(s.data(), s.size());
    if (bad) *bad = n;
    return s;
}

int main() {
    // Valid text is left alone: ASCII, Cyrillic, the euro sign, an emoji, the first and last character of
    // every valid range.
    const std::string ok = "{\"name\":\"Привіт €\xf0\x9f\x98\x80\",\"x\":\"\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
                           "\xee\x80\x80\xf0\x90\x80\x80\xf4\x8f\xbf\xbf\"}";
    size_t bad = 99;
    assert(utf8_valid(ok.data(), ok.size()));
    assert(scrubbed(ok, &bad) == ok && bad == 0);

    // A rule name cut by strncpy inside a character (dual-chip rule.list, older firmware's rule.added): the half
    // character becomes '?', the frame keeps its length and its quotes.
    const std::string cut = "{\"name\":\"аб\xd0\",\"dsl\":\"x\"}";
    assert(!utf8_valid(cut.data(), cut.size()));
    assert(scrubbed(cut, &bad) == "{\"name\":\"аб?\",\"dsl\":\"x\"}" && bad == 1);

    // Dual-chip rule.list cuts a rule's text at 127 bytes: here inside the euro sign (3 bytes, 2 kept).
    const std::string text = std::string(125, 'a') + "€";
    const std::string row = "{\"dsl\":\"" + text.substr(0, 127) + "\"}";
    assert(scrubbed(row, &bad) == "{\"dsl\":\"" + std::string(125, 'a') + "??\"}" && bad == 2);

    // A log line or a REST-uploaded script with stray bytes: overlong forms, surrogates, past U+10FFFF, a lone
    // continuation byte, 0xFF. One '?' per byte, and nothing else changes.
    for (const char* b : {"\xc0\x80", "\xc1\xbf", "\xe0\x9f\xbf", "\xed\xa0\x80", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
                          "\xf5\x80\x80\x80", "\x80", "\xbf\xbf", "\xff", "\xfe\xff"}) {
        const std::string in = std::string("{\"m\":\"x") + b + "y\"}";
        const std::string want = "{\"m\":\"x" + std::string(std::strlen(b), '?') + "y\"}";
        assert(!utf8_valid(in.data(), in.size()));
        assert(scrubbed(in, &bad) == want && bad == std::strlen(b));
        assert(utf8_valid(want.data(), want.size()));
    }

    // A character cut by the end of the buffer (a frame budget, not a NUL): the guard never reads past `len`.
    const std::string tail = "{\"a\":1}\xe2\x82";
    assert(scrubbed(tail, &bad) == "{\"a\":1}??" && bad == 2);
    assert(utf8_valid("", 0) && utf8_scrub(nullptr, 0) == 0);

    std::printf("utf8_guard_tests: ok\n");
    return 0;
}
