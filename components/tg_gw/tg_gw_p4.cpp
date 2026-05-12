// components/tg_gw/tg_gw_p4.cpp
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tg_gw.h"
#include "hap_protocol.h"
#include "hap_session.h"
#include "hap_json.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "tg_gw_p4";

void tg_gw_init(void) {
    ESP_LOGI(TAG, "tg_gw_p4 init — telegram routes via HAP to S3");
}

static bool send_one(HapMsgType type, const uint8_t* buf, uint16_t len) {
    HapFrame f{};
    f.type        = type;
    f.seq         = hap_session_next_seq();
    f.flags       = 0;
    f.payload     = buf;
    f.payload_len = len;
    return hap_session_send(f);
}

bool tg_gw_settoken(const char* token) {
    if (!token) return false;
    size_t n = strnlen(token, 97);
    if (n == 0 || n > 96) {
        ESP_LOGW(TAG, "settoken: bad len %u", (unsigned)n);
        return false;
    }
    HapTgSettoken m{};
    memcpy(m.token, token, n);
    m.token[n] = '\0';
    m.token_len = (uint8_t)n;

    uint8_t buf[110];
    uint16_t len = 0;
    if (!hap_pack_tg_settoken(buf, sizeof(buf), &len, m)) return false;
    bool ok = send_one(HapMsgType::TG_SETTOKEN, buf, len);
    ESP_LOGI(TAG, "TG_SETTOKEN forwarded len=%u (token masked)", (unsigned)n);
    return ok;
}

bool tg_gw_setchat(const char* chat_id_str) {
    if (!chat_id_str) return false;
    size_t n = strnlen(chat_id_str, 33);
    if (n == 0 || n > 32) {
        ESP_LOGW(TAG, "setchat: bad len %u", (unsigned)n);
        return false;
    }
    HapTgSetchat m{};
    memcpy(m.chat, chat_id_str, n);
    m.chat[n] = '\0';
    m.chat_len = (uint8_t)n;

    uint8_t buf[40];
    uint16_t len = 0;
    if (!hap_pack_tg_setchat(buf, sizeof(buf), &len, m)) return false;
    bool ok = send_one(HapMsgType::TG_SETCHAT, buf, len);
    ESP_LOGI(TAG, "TG_SETCHAT forwarded chat=%s", m.chat);
    return ok;
}

bool tg_gw_send(const char* text,
                const char* chat_id_or_null,
                const char* parse_mode_or_null) {
    if (!text) return false;
    size_t tn = strnlen(text, 3073);
    if (tn == 0 || tn > 3072) {
        ESP_LOGW(TAG, "send: bad text len %u", (unsigned)tn);
        return false;
    }
    HapTgSend m{};
    if (chat_id_or_null && *chat_id_or_null) {
        size_t cn = strnlen(chat_id_or_null, 33);
        if (cn > 32) return false;
        memcpy(m.chat, chat_id_or_null, cn);
        m.chat[cn] = '\0';
        m.chat_len = (uint8_t)cn;
    }
    if (parse_mode_or_null && *parse_mode_or_null) {
        size_t pn = strnlen(parse_mode_or_null, 33);
        if (pn > 32) return false;
        memcpy(m.parse_mode, parse_mode_or_null, pn);
        m.parse_mode[pn] = '\0';
        m.parse_mode_len = (uint8_t)pn;
    }
    memcpy(m.text, text, tn);
    m.text[tn] = '\0';
    m.text_len = (uint16_t)tn;

    static uint8_t buf[3200];
    uint16_t len = 0;
    if (!hap_pack_tg_send(buf, sizeof(buf), &len, m)) {
        ESP_LOGW(TAG, "send: pack failed");
        return false;
    }
    bool ok = send_one(HapMsgType::TG_SEND, buf, len);
    ESP_LOGI(TAG, "TG_SEND forwarded text_len=%u chat_override=%s parse_mode=%s",
             (unsigned)tn,
             m.chat_len ? m.chat : "(default)",
             m.parse_mode_len ? m.parse_mode : "(none)");
    return ok;
}
