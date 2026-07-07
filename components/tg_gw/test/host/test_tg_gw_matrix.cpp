// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Characterization MATRIX host tests for the tg_gw (Telegram gateway)
// component. Complements test_tg_gw_helpers.cpp (which pins the two S3
// PURE security helpers json_escape + parse_mode) by covering the rest of
// the component's PURE, deterministic surface:
//
//   • P4 public-API accept-gates (tg_gw_p4.cpp): token / chat / send
//     length + null + boundary validation — the config + send-routing
//     acceptance logic that decides whether a verb is forwarded over HAP.
//   • Cross-core token cap agreement (F8-T19): P4's accept cap and S3's
//     handle_settoken reject cap must BOTH be TG_TOKEN_MAX so an 81..96
//     char token can't be accepted on one side and dropped on the other.
//   • S3 tg_perform_send composition (tg_gw_s3.cpp): chat selection
//     (override / stored default / drop), URL building, and JSON body
//     assembly with & without parse_mode.
//   • S3 parse_mode whitelist as the SECOND gate behind P4's length-only
//     check (P4 forwards any <=32-char parse_mode; S3 filters to the three
//     accepted values).
//   • json_escape edges the helpers test omits: null / zero-cap guards,
//     empty input, 4-byte UTF-8 keep + truncation back-off, atomic escape
//     truncation (never emit a dangling backslash), exact-fit boundary.
//   • End-to-end injection: attacker chat_id + text through json_escape
//     into the real body template still yields ONE well-formed JSON object.
//
// Neither tg_gw TU links on the host (P4 → hap_protocol/hap_session/hap_json;
// S3 → hap_json/esp_http_client/nvs/FreeRTOS), and there is no stub surface,
// so — matching test_tg_gw_helpers.cpp and the zap_store precedent — every
// pure fragment under test is MIRRORED here verbatim and marked
// "mirror of <file>:<fn> — keep in sync".
#include "tg_gw.h"        // TG_TOKEN_MAX — the shared cross-core token cap
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
//    sync. Non-empty parse_mode must be exactly one of the three values.
static bool parse_mode_ok(const char* pm) {
    return strcmp(pm, "Markdown")   == 0 ||
           strcmp(pm, "MarkdownV2") == 0 ||
           strcmp(pm, "HTML")       == 0;
}

// Does a NUL-terminated string contain an UNESCAPED '"' (one not immediately
// preceded by a backslash)? That is the JSON-framing break json_escape output
// must never contain. (Same helper as test_tg_gw_helpers.cpp.)
static bool has_unescaped_quote(const char* s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) return true;
    }
    return false;
}

// ── mirror of tg_gw_p4.cpp:tg_gw_settoken accept-gate — keep in sync ───────
// The real fn then packs + HAP-sends; on the host we pin the gate only.
static bool p4_settoken_accepts(const char* token) {
    if (!token) return false;
    size_t n = strnlen(token, TG_TOKEN_MAX + 1);
    return !(n == 0 || n > TG_TOKEN_MAX);
}

// ── mirror of tg_gw_p4.cpp:tg_gw_setchat accept-gate — keep in sync ────────
static bool p4_setchat_accepts(const char* chat) {
    if (!chat) return false;
    size_t n = strnlen(chat, 33);
    return !(n == 0 || n > 32);
}

// ── mirror of tg_gw_p4.cpp:tg_gw_send accept-gate — keep in sync ───────────
// Validation only (the real fn then takes s_send_mutex, packs, HAP-sends).
// NOTE: parse_mode is LENGTH-checked only here; its whitelist lives S3-side.
static bool p4_send_accepts(const char* text, const char* chat, const char* pm) {
    if (!text) return false;
    size_t tn = strnlen(text, 3073);
    if (tn == 0 || tn > 3072) return false;
    if (chat && *chat) { if (strnlen(chat, 33) > 32) return false; }
    if (pm   && *pm)   { if (strnlen(pm,   33) > 32) return false; }
    return true;
}

// ── mirror of tg_gw_s3.cpp:tg_gw_handle_settoken cap-reject — keep in sync ─
// S3 rejects when the unpacked token_len exceeds the shared cap.
static bool s3_token_len_accepts(unsigned token_len) {
    return token_len <= TG_TOKEN_MAX;
}

// ── mirror of tg_gw_s3.cpp:tg_perform_send chat selection — keep in sync ───
// Override wins; else the stored default; else "" (empty → the send drops).
static const char* s3_pick_chat(uint8_t override_len, const char* override_chat,
                                bool have_chat, const char* default_chat) {
    return override_len ? override_chat : (have_chat ? default_chat : "");
}

// ── mirror of tg_gw_s3.cpp:tg_perform_send URL build — keep in sync ────────
static int s3_build_url(char* url, size_t cap, const char* token) {
    return snprintf(url, cap,
                    "https://api.telegram.org/bot%s/sendMessage", token);
}

// ── mirror of tg_gw_s3.cpp:tg_perform_send body assembly — keep in sync ────
// Fields are already json_escape'd; assembled via snprintf. Returns the
// snprintf value (>= cap ⇒ the source treats it as body overflow → drop).
static int s3_build_body(char* body, size_t cap,
                         const char* chat_esc, const char* text_esc,
                         bool want_pm, const char* pm_esc) {
    if (want_pm)
        return snprintf(body, cap,
            "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"%s\"}",
            chat_esc, text_esc, pm_esc);
    return snprintf(body, cap,
            "{\"chat_id\":\"%s\",\"text\":\"%s\"}", chat_esc, text_esc);
}

// Fill buf with n copies of c + NUL; returns buf. For boundary-length args.
static const char* fill(char* buf, size_t n, char c) {
    for (size_t i = 0; i < n; i++) buf[i] = c;
    buf[n] = '\0';
    return buf;
}

int main() {
    printf("test_tg_gw_matrix (P4 gates / cross-core cap / S3 compose / escape edges)\n");
    char big[3200];
    char out[4096];

    // ── A. P4 tg_gw_settoken accept-gate ──────────────────────────────────
    {
        CHECK(!p4_settoken_accepts(nullptr), "settoken: null token rejected");
        CHECK(!p4_settoken_accepts(""),      "settoken: empty token rejected");
        CHECK(p4_settoken_accepts("x"),      "settoken: 1-char token accepted");
        CHECK(p4_settoken_accepts("123456789:AAExample-Token_Value12345678901"),
                                             "settoken: realistic bot token accepted");
        CHECK(p4_settoken_accepts(fill(big, TG_TOKEN_MAX, 'x')),
                                             "settoken: exactly TG_TOKEN_MAX accepted");
        CHECK(!p4_settoken_accepts(fill(big, TG_TOKEN_MAX + 1, 'x')),
                                             "settoken: TG_TOKEN_MAX+1 rejected");
        CHECK(!p4_settoken_accepts(fill(big, 200, 'x')),
                                             "settoken: 200-char token rejected");
    }

    // ── B. P4 tg_gw_setchat accept-gate ───────────────────────────────────
    {
        CHECK(!p4_setchat_accepts(nullptr),          "setchat: null rejected");
        CHECK(!p4_setchat_accepts(""),               "setchat: empty rejected");
        CHECK(p4_setchat_accepts("1"),               "setchat: 1-char accepted");
        CHECK(p4_setchat_accepts("-1001234567890"),  "setchat: supergroup id accepted");
        CHECK(p4_setchat_accepts("@channelusername"),"setchat: @username accepted");
        CHECK(p4_setchat_accepts(fill(big, 32, '9')),"setchat: 32-char boundary accepted");
        CHECK(!p4_setchat_accepts(fill(big, 33, '9')),"setchat: 33-char rejected");
    }

    // ── C. P4 tg_gw_send accept-gate (text + optional overrides) ──────────
    {
        char cbuf[64]; char pbuf[64];
        CHECK(!p4_send_accepts(nullptr, nullptr, nullptr), "send: null text rejected");
        CHECK(!p4_send_accepts("", nullptr, nullptr),      "send: empty text rejected");
        CHECK(p4_send_accepts("hi", nullptr, nullptr),     "send: text, no overrides accepted");
        CHECK(p4_send_accepts(fill(big, 3072, 'a'), nullptr, nullptr),
                                                           "send: 3072-char text boundary accepted");
        CHECK(!p4_send_accepts(fill(big, 3073, 'a'), nullptr, nullptr),
                                                           "send: 3073-char text rejected");
        CHECK(p4_send_accepts("hi", "12345", nullptr),     "send: chat override accepted");
        CHECK(p4_send_accepts("hi", "", nullptr),          "send: empty chat override = no override, accepted");
        CHECK(p4_send_accepts("hi", nullptr, "HTML"),      "send: parse_mode override accepted");
        CHECK(p4_send_accepts("hi", nullptr, ""),          "send: empty parse_mode = omit, accepted");
        CHECK(p4_send_accepts("hi", nullptr, "XML"),       "send: bad-content parse_mode passes P4 length gate");
        CHECK(p4_send_accepts("hi", fill(cbuf, 32, 'c'), nullptr),
                                                           "send: 32-char chat override accepted");
        CHECK(!p4_send_accepts("hi", fill(cbuf, 33, 'c'), nullptr),
                                                           "send: 33-char chat override rejected");
        CHECK(p4_send_accepts("hi", nullptr, fill(pbuf, 32, 'p')),
                                                           "send: 32-char parse_mode override accepted");
        CHECK(!p4_send_accepts("hi", nullptr, fill(pbuf, 33, 'p')),
                                                           "send: 33-char parse_mode override rejected");
    }

    // ── D. Cross-core token cap agreement (F8-T19) ────────────────────────
    {
        CHECK(TG_TOKEN_MAX == 80, "F8-T19: shared token cap is 80 (a bump must review BOTH cores)");
        CHECK(p4_settoken_accepts(fill(big, 80, 'x')) && s3_token_len_accepts(80),
              "cap: 80-char token accepted by BOTH cores");
        CHECK(!p4_settoken_accepts(fill(big, 81, 'x')) && !s3_token_len_accepts(81),
              "cap: 81-char token rejected by BOTH cores");
        CHECK(!p4_settoken_accepts(fill(big, 96, 'x')) && !s3_token_len_accepts(96),
              "cap: 96-char token (old P4 max) rejected by BOTH cores");
        // Documented asymmetry at the empty end: P4 rejects empty at the gate;
        // S3's cap check accepts len 0 (persists empty) but tg_perform_send
        // later drops on token[0]=='\0'. Net: empty token never sends either way.
        CHECK(!p4_settoken_accepts("") && s3_token_len_accepts(0),
              "cap: empty token — P4 rejects at gate, S3 caps-ok (dropped later at send)");
    }

    // ── E. S3 parse_mode whitelist = second gate behind P4 length gate ────
    {
        CHECK(p4_send_accepts("hi", nullptr, "HTML") && parse_mode_ok("HTML"),
              "parse_mode HTML: passes P4 length AND S3 whitelist");
        CHECK(p4_send_accepts("hi", nullptr, "Markdown") && parse_mode_ok("Markdown"),
              "parse_mode Markdown: passes both");
        CHECK(p4_send_accepts("hi", nullptr, "MarkdownV2") && parse_mode_ok("MarkdownV2"),
              "parse_mode MarkdownV2: passes both");
        CHECK(p4_send_accepts("hi", nullptr, "XML") && !parse_mode_ok("XML"),
              "parse_mode XML: passes P4 length but S3 whitelist DROPS it");
        CHECK(p4_send_accepts("hi", nullptr, "html") && !parse_mode_ok("html"),
              "parse_mode lowercase html: passes P4 length but S3 DROPS (case-sensitive)");
    }

    // ── F. S3 chat selection (override / default / drop) ──────────────────
    {
        const char* dflt = "@defaultchat";
        CHECK(strcmp(s3_pick_chat(5, "12345", true, dflt), "12345") == 0,
              "chat select: override wins over stored default");
        CHECK(strcmp(s3_pick_chat(0, "", true, dflt), dflt) == 0,
              "chat select: no override falls back to stored default");
        CHECK(s3_pick_chat(0, "", false, "")[0] == '\0',
              "chat select: no override + no default → empty (send drops)");
        CHECK(s3_pick_chat(3, "789", false, "")[0] != '\0',
              "chat select: override with no default still sends");
    }

    // ── G. S3 URL build ───────────────────────────────────────────────────
    {
        char url[200];
        int n = s3_build_url(url, sizeof(url), "123456789:AAExampleToken");
        CHECK(n > 0 && (size_t)n < sizeof(url), "url: builds within url[200]");
        CHECK(strcmp(url, "https://api.telegram.org/bot123456789:AAExampleToken/sendMessage") == 0,
              "url: bot<token>/sendMessage framing");
        int n2 = s3_build_url(url, sizeof(url), fill(big, TG_TOKEN_MAX, 't'));
        CHECK(n2 > 0 && (size_t)n2 < sizeof(url),
              "url: max-length token still fits url[200] (overflow guard never trips in-spec)");
    }

    // ── H. S3 JSON body assembly ──────────────────────────────────────────
    {
        char body[3500];
        int b1 = s3_build_body(body, sizeof(body), "123", "hi", false, nullptr);
        CHECK(b1 > 0 && strcmp(body, "{\"chat_id\":\"123\",\"text\":\"hi\"}") == 0,
              "body: no-parse_mode framing");
        int b2 = s3_build_body(body, sizeof(body), "123", "hi", true, "HTML");
        CHECK(b2 > 0 && strcmp(body,
              "{\"chat_id\":\"123\",\"text\":\"hi\",\"parse_mode\":\"HTML\"}") == 0,
              "body: with-parse_mode framing");
        char tiny[8];
        int b3 = s3_build_body(tiny, sizeof(tiny), "123", "hello", false, nullptr);
        CHECK(b3 > 0 && (size_t)b3 >= sizeof(tiny),
              "body: overflow detected (snprintf return >= cap)");
    }

    // ── I. End-to-end injection through the real body template ────────────
    {
        // Attacker tries to break {"chat_id":"..."} framing via a crafted
        // chat id AND text. After json_escape neither field may contain an
        // unescaped '"'; the assembled body stays ONE well-formed object.
        char chat_esc[80], text_esc[3200], body[3500];
        json_escape("123\",\"x\":\"y", chat_esc, sizeof(chat_esc));
        json_escape("a\"b\nc",         text_esc, sizeof(text_esc));
        CHECK(!has_unescaped_quote(chat_esc), "injection: chat field has no unescaped quote");
        CHECK(!has_unescaped_quote(text_esc), "injection: text field has no unescaped quote");
        int bn = s3_build_body(body, sizeof(body), chat_esc, text_esc, false, nullptr);
        CHECK(bn > 0 && strcmp(body,
              "{\"chat_id\":\"123\\\",\\\"x\\\":\\\"y\",\"text\":\"a\\\"b\\nc\"}") == 0,
              "injection: assembled body is one well-formed JSON object");
    }

    // ── J. json_escape edges the helpers test omits ───────────────────────
    {
        CHECK(json_escape(nullptr, out, sizeof(out)) == 0, "escape: null in → 0");
        CHECK(json_escape("hi", nullptr, 10) == 0,         "escape: null out → 0");
        CHECK(json_escape("hi", out, 0) == 0,              "escape: zero cap → 0");

        out[0] = 'Z';
        size_t we = json_escape("", out, sizeof(out));
        CHECK(we == 0 && out[0] == '\0', "escape: empty input → 0 + NUL");

        size_t wf = json_escape("abc", out, 4);
        CHECK(wf == 3 && strcmp(out, "abc") == 0, "escape: exact-fit (cap==len+1) keeps all");

        size_t wo = json_escape("abcd", out, 4);
        CHECK(wo == 3 && strcmp(out, "abc") == 0, "escape: one-over truncates trailing ASCII");

        // Atomic escape truncation: a 2-byte escape (\") that doesn't fit is
        // dropped WHOLE — never a dangling lone backslash.
        size_t wa = json_escape("A\"", out, 3);
        CHECK(wa == 1 && strcmp(out, "A") == 0, "escape: partial \\\" escape dropped whole");
        CHECK(strchr(out, '\\') == nullptr, "escape: no lone backslash after atomic drop");

        // 4-byte UTF-8 (U+1F600 emoji = F0 9F 98 80).
        const char* emoji = "x\xf0\x9f\x98\x80";
        size_t w4 = json_escape(emoji, out, 8);
        CHECK(w4 == 5, "escape: 4-byte UTF-8 kept when room");
        CHECK((unsigned char)out[1] == 0xF0 && (unsigned char)out[2] == 0x9F &&
              (unsigned char)out[3] == 0x98 && (unsigned char)out[4] == 0x80,
              "escape: 4-byte emoji bytes intact");
        size_t w4t = json_escape(emoji, out, 5);
        CHECK(w4t == 1 && strcmp(out, "x") == 0,
              "escape: incomplete 4-byte seq (lead+2 of 3 conts) dropped");
    }

    if (s_failures) { printf("FAILED: %d check(s)\n", s_failures); }
    else            { printf("ALL PASS\n"); }
    return s_failures ? 1 : 0;
}
