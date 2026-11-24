// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tg_gw_body.h"

#include <cstdio>
#include <cstring>

namespace {

bool secret_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

// JSON-escapes `in` into out[0..cap), NUL-terminated. Escapes " \ \n \r \t, drops other control
// bytes. When it runs out of room it stops on a whole UTF-8 character (a cut multi-byte sequence
// makes Telegram refuse the body). *cut says whether input was left over.
size_t json_escape(const char* in, char* out, size_t cap, bool* cut) {
    size_t w = 0;
    *cut = false;
    for (size_t i = 0; in[i]; i++) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        char one[2] = {static_cast<char>(c), '\0'};
        const char* esc = one;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:   if (c < 0x20) continue;
        }
        const size_t n = std::strlen(esc);
        if (w + n + 1 > cap) { *cut = true; break; }
        std::memcpy(out + w, esc, n);
        w += n;
    }
    if (*cut && w > 0) {   // back off over a trailing incomplete UTF-8 sequence
        size_t k = w, cont = 0;
        while (k > 0 && (static_cast<unsigned char>(out[k - 1]) & 0xC0) == 0x80) { k--; cont++; }
        if (k > 0) {
            const unsigned char lead = static_cast<unsigned char>(out[k - 1]);
            const size_t need = (lead & 0x80) == 0x00 ? 0 : (lead & 0xE0) == 0xC0 ? 1
                              : (lead & 0xF0) == 0xE0 ? 2 : (lead & 0xF8) == 0xF0 ? 3 : 99;
            if (cont < need) w = k - 1;
        }
    }
    out[w] = '\0';
    return w;
}

}  // namespace

bool tg_token_ok(const char* t) {
    if (!t || std::strlen(t) > TG_TOKEN_MAX) return false;
    size_t i = 0;
    while (t[i] >= '0' && t[i] <= '9') i++;
    if (i == 0 || t[i] != ':' || !t[i + 1]) return false;
    for (i++; t[i]; i++)
        if (!secret_char(t[i])) return false;
    return true;
}

bool tg_chat_ok(const char* c) {
    if (!c || !c[0] || std::strlen(c) > TG_CHAT_MAX) return false;
    if (c[0] == '@') {
        if (!c[1]) return false;
        for (size_t i = 1; c[i]; i++)
            if (!secret_char(c[i]) || c[i] == '-') return false;
        return true;
    }
    size_t i = c[0] == '-' ? 1 : 0;
    if (!c[i]) return false;
    for (; c[i]; i++)
        if (c[i] < '0' || c[i] > '9') return false;
    return true;
}

int tg_build_body(const char* text, const char* chat, const char* pm, char* out, size_t cap) {
    if (!text || !out || !tg_chat_ok(chat)) return -1;
    const bool with_pm = pm && pm[0];
    if (with_pm && std::strcmp(pm, "Markdown") != 0 && std::strcmp(pm, "MarkdownV2") != 0 &&
        std::strcmp(pm, "HTML") != 0)
        return -1;
    int n = std::snprintf(out, cap, "{\"chat_id\":\"%s\",\"text\":\"", chat);
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    const size_t room = cap - static_cast<size_t>(n);
    const size_t lim = room < TG_TEXT_ESC_MAX + 1 ? room : TG_TEXT_ESC_MAX + 1;
    bool cut = false;
    n += static_cast<int>(json_escape(text, out + n, lim, &cut));
    if (cut && lim == room) return -1;   // cut by the buffer, not by the message limit
    const int m = with_pm ? std::snprintf(out + n, cap - n, "\",\"parse_mode\":\"%s\"}", pm)
                          : std::snprintf(out + n, cap - n, "\"}");
    if (m < 0 || static_cast<size_t>(m) >= cap - n) return -1;
    return n + m;
}

bool tg_send_retry(bool answered, int status) {
    return !answered || status == 429 || status >= 500;
}
