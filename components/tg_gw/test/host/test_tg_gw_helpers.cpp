// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host tests for tg_gw_s3.cpp T19 PURE security helpers.
//
// tg_gw_s3.cpp links hap_json + esp_http_client + nvs + FreeRTOS, so it
// cannot link on the host and a whole-TU #include would need a large stub
// surface. The two pieces under test are PURE, so — matching the
// zap_store/test/host precedent — we MIRROR their EXACT logic and keep it
// in lockstep with the source:
//
//   F9-T19 json_escape:  escapes ", \, \n, \r, \t and drops other control
//                        bytes; on truncation backs off to the last
//                        complete UTF-8 code-point boundary. This is the
//                        defence against JSON-framing injection through the
//                        HAP/NVS/Lua-sourced chat-id and parse_mode fields.
//   F7-T19 parse_mode:   whitelist — Markdown / MarkdownV2 / HTML only.
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── mirror of tg_gw_s3.cpp:json_escape — keep in sync ─────────────────────
static size_t json_escape(const char* in, char* out, size_t cap) {
    if (!in || !out || cap == 0) return 0;
    size_t wi = 0;
    bool truncated = false;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        const char* esc = nullptr; char buf2[3];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) continue;  // drop other controls
                buf2[0] = (char)c; buf2[1] = '\0'; esc = buf2; break;
        }
        size_t n = strlen(esc);
        if (wi + n + 1 > cap) { truncated = true; break; }
        memcpy(out + wi, esc, n); wi += n;
    }
    if (truncated && wi > 0) {
        size_t k = wi;
        size_t cont = 0;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) {
            k--; cont++;
        }
        if (k > 0) {
            unsigned char lead = (unsigned char)out[k - 1];
            size_t need;
            if      ((lead & 0x80) == 0x00) need = 0;   // ASCII
            else if ((lead & 0xE0) == 0xC0) need = 1;
            else if ((lead & 0xF0) == 0xE0) need = 2;
            else if ((lead & 0xF8) == 0xF0) need = 3;
            else                            need = (size_t)-1;  // invalid lead
            if (need == (size_t)-1 || cont < need) {
                wi = k - 1;
            }
        }
    }
    out[wi] = '\0';
    return wi;
}

// ── mirror of tg_gw_s3.cpp:tg_perform_send parse_mode whitelist — keep in
//    sync. The source rejects (drops the send) when the configured
//    parse_mode is non-empty AND not one of the three accepted values.
static bool parse_mode_ok(const char* pm) {
    return strcmp(pm, "Markdown")   == 0 ||
           strcmp(pm, "MarkdownV2") == 0 ||
           strcmp(pm, "HTML")       == 0;
}

// Helper: does a NUL-terminated string contain an UNESCAPED '"' — i.e. a
// '"' not immediately preceded by a backslash? That is the JSON-framing
// break we must never emit from json_escape output.
static bool has_unescaped_quote(const char* s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) return true;
    }
    return false;
}

int main() {
    printf("test_tg_gw_helpers (T19 F9 json_escape / F7 parse_mode)\n");
    char out[256];

    // ── F9-T19: json_escape basic escaping ───────────────────────────────
    {
        json_escape("hello", out, sizeof(out));
        CHECK(strcmp(out, "hello") == 0, "plain text unchanged");

        json_escape("a\"b", out, sizeof(out));
        CHECK(strcmp(out, "a\\\"b") == 0, "double-quote escaped");

        json_escape("a\\b", out, sizeof(out));
        CHECK(strcmp(out, "a\\\\b") == 0, "backslash escaped");

        json_escape("a\nb\tc\rd", out, sizeof(out));
        CHECK(strcmp(out, "a\\nb\\tc\\rd") == 0, "\\n \\t \\r escaped");

        // other control bytes are dropped
        json_escape("a\x01\x1f" "b", out, sizeof(out));
        CHECK(strcmp(out, "ab") == 0, "other control bytes dropped");
    }

    // ── F9-T19: the chat-id JSON injection case ──────────────────────────
    {
        // chat = 123","x":"y  — an attacker trying to close the chat_id
        // string and inject extra JSON keys. After escaping there must be
        // NO unescaped '"' that could break the {"chat_id":"..."} framing.
        const char* attack = "123\",\"x\":\"y";
        json_escape(attack, out, sizeof(out));
        CHECK(!has_unescaped_quote(out), "injection: no unescaped quote in output");
        CHECK(strcmp(out, "123\\\",\\\"x\\\":\\\"y") == 0,
              "injection: every quote backslash-escaped");
        // sanity: dropping it into a body string yields a single JSON string
        char body[300];
        snprintf(body, sizeof(body), "{\"chat_id\":\"%s\"}", out);
        CHECK(strcmp(body, "{\"chat_id\":\"123\\\",\\\"x\\\":\\\"y\"}") == 0,
              "injection: framed body stays well-formed");
    }

    // ── F9-T19: UTF-8 truncation boundary back-off ───────────────────────
    {
        // "ab" + é (0xC3 0xA9, a 2-byte code point). With cap chosen so the
        // multibyte char lands exactly on the truncation edge, the partial
        // lead byte must be dropped cleanly — output ends at "ab", valid.
        const char* s = "ab\xc3\xa9";   // a, b, é
        // cap must hold 'a','b' + NUL but cut inside é. After writing 'a','b'
        // wi==2; é's lead 0xC3 needs wi+1+1<=cap → cap>=4 to fit lead+NUL.
        // Use cap=4: 'a'(wi1) 'b'(wi2) then lead 0xC3 n==1 → 2+1+1>4? no, ==4
        // not >4, so lead is written (wi3); cont 0xA9 n==1 → 3+1+1=5>4 → break.
        // Back-off: out[2]==0xC3 is a lead expecting 1 cont, cont==0 → drop → wi=2.
        size_t w = json_escape(s, out, 4);
        CHECK(w == 2, "utf8 edge: backed off to last complete code point");
        CHECK(strcmp(out, "ab") == 0, "utf8 edge: dropped partial lead byte");
        CHECK((unsigned char)out[2] == '\0' || out[2] == 0,
              "utf8 edge: NUL-terminated at boundary");

        // With enough room the whole é survives intact.
        size_t w2 = json_escape(s, out, 8);
        CHECK(w2 == 4, "utf8: full 2-byte code point kept when room");
        CHECK((unsigned char)out[2] == 0xC3 && (unsigned char)out[3] == 0xA9,
              "utf8: é bytes intact");

        // 3-byte code point (e.g. U+20AC € = E2 82 AC) cut after lead+1 cont:
        // off-by-one in the wi=k-1 region is the bug class. cap chosen so the
        // lead + one continuation fit but not the second → drop all 3.
        const char* e = "x\xe2\x82\xac";  // x, €
        // x(wi1) lead 0xE2(wi2) cont 0x82(wi3); next cont 0xAC: 3+1+1=5>cap4
        // → break. Back-off: out[2]=0x82 cont (cont=1), out[1]=0xE2 lead
        // need=2 > cont=1 → drop lead+conts → wi = k-1 = 1.
        size_t w3 = json_escape(e, out, 4);
        CHECK(w3 == 1 && strcmp(out, "x") == 0,
              "utf8 3-byte: incomplete seq (lead+1 of 2 conts) dropped");
    }

    // ── F7-T19: parse_mode whitelist ─────────────────────────────────────
    {
        CHECK(parse_mode_ok("Markdown"),   "accept Markdown");
        CHECK(parse_mode_ok("MarkdownV2"), "accept MarkdownV2");
        CHECK(parse_mode_ok("HTML"),       "accept HTML");
        CHECK(!parse_mode_ok("markdown"),  "reject lowercase markdown");
        CHECK(!parse_mode_ok("html"),      "reject lowercase html");
        CHECK(!parse_mode_ok("XML"),       "reject unknown XML");
        CHECK(!parse_mode_ok(""),          "reject empty as non-whitelisted");
        CHECK(!parse_mode_ok("Markdown ; rm -rf"), "reject injection-y value");
    }

    if (s_failures) { printf("FAILED: %d check(s)\n", s_failures); return 1; }
    printf("ALL PASS\n");
    return 0;
}
