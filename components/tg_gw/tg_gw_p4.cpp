// components/tg_gw/tg_gw_p4.cpp
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tg_gw.h"
#include "hap_protocol.h"
#include "hap_session.h"
#include "hap_json.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

static const char* TAG = "tg_gw_p4";

// F10-T19: tg_gw_send packs into a static ~3.2 KB buffer (and a static
// HapTgSend) to keep it off the caller's stack (hap_slave canary). Two
// tasks (the rules engine + the Lua engine) can call tg_gw_send
// concurrently, which would corrupt the frame mid-pack/send. Serialize
// the whole pack+send with a mutex — same pattern mqtt_gw_p4 uses.
// Created ONCE in tg_gw_init (single-threaded boot; no lazy-create race).
static SemaphoreHandle_t s_send_mutex = nullptr;

void tg_gw_init(void) {
    if (!s_send_mutex) s_send_mutex = xSemaphoreCreateMutex();
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
    // F8-T19: use the shared TG_TOKEN_MAX so P4 and S3 agree on the cap.
    // Previously P4 accepted up to 96 while S3 rejected >80 → an
    // 81–96-char token was forwarded then silently dropped on S3.
    size_t n = strnlen(token, TG_TOKEN_MAX + 1);
    if (n == 0 || n > TG_TOKEN_MAX) {
        ESP_LOGW(TAG, "settoken: bad len %u (max %u)", (unsigned)n,
                 (unsigned)TG_TOKEN_MAX);
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
    // Validate the optional override args before touching shared state.
    size_t cn = 0, pn = 0;
    if (chat_id_or_null && *chat_id_or_null) {
        cn = strnlen(chat_id_or_null, 33);
        if (cn > 32) return false;
    }
    if (parse_mode_or_null && *parse_mode_or_null) {
        pn = strnlen(parse_mode_or_null, 33);
        if (pn > 32) return false;
    }

    // F10-T19: the HapTgSend (~3.2 KB: text[3072]+chat+parse_mode) and
    // the pack buffer are static to stay off the caller's stack. Serialize
    // the whole pack+send under s_send_mutex so a concurrent tg_gw_send
    // from another task (rules vs Lua) can't corrupt the shared buffers
    // mid-pack/send.
    static HapTgSend m;
    static uint8_t   buf[3200];
    if (!s_send_mutex || xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "send: mutex contention — drop");
        return false;
    }

    memset(&m, 0, sizeof(m));
    if (cn) {
        memcpy(m.chat, chat_id_or_null, cn);
        m.chat[cn] = '\0';
        m.chat_len = (uint8_t)cn;
    }
    if (pn) {
        memcpy(m.parse_mode, parse_mode_or_null, pn);
        m.parse_mode[pn] = '\0';
        m.parse_mode_len = (uint8_t)pn;
    }
    memcpy(m.text, text, tn);
    m.text[tn] = '\0';
    m.text_len = (uint16_t)tn;

    uint16_t len = 0;
    if (!hap_pack_tg_send(buf, sizeof(buf), &len, m)) {
        xSemaphoreGive(s_send_mutex);
        ESP_LOGW(TAG, "send: pack failed");
        return false;
    }
    bool ok = send_one(HapMsgType::TG_SEND, buf, len);
    ESP_LOGI(TAG, "TG_SEND forwarded text_len=%u chat_override=%s parse_mode=%s",
             (unsigned)tn,
             m.chat_len ? m.chat : "(default)",
             m.parse_mode_len ? m.parse_mode : "(none)");
    xSemaphoreGive(s_send_mutex);
    return ok;
}
