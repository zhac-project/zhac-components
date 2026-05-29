// components/tg_gw/tg_gw_s3.cpp
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tg_gw.h"
#include "hap_json.h"
#include "task_stacks.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG       = "tg_gw_s3";
static const char* NVS_NS    = "zhac";
static const char* NVS_TOKEN = "tg_token";
static const char* NVS_CHAT  = "tg_chat";

// F45 (FINDINGS.md): documented max Telegram bot-token length. Real tokens are
// `<bot_id>:<35-char-secret>` ≈ 46 chars; 80 gives generous headroom. Reject
// over-long tokens on SET so they can't silently truncate in the send buffers
// (a truncated token breaks delivery, not security — HTTPS is verified).
static constexpr uint16_t TG_TOKEN_MAX = 80;

static SemaphoreHandle_t s_cfg_mutex = nullptr;

// TG_SEND is offloaded onto a dedicated worker. The HAP frame thread
// (8 KB stack) cannot host the mbedtls TLS handshake to
// api.telegram.org — SHA + x509 verify trips the stack canary. The
// worker has its own 12 KB stack (zhac::stack::kTgWorker) and serialises
// outgoing HTTPS posts.
static QueueHandle_t s_tg_q = nullptr;
static constexpr UBaseType_t kTgQueueDepth = 4;

static void tg_perform_send(const HapTgSend& m);
static void tg_worker_task(void*);

void tg_gw_init(void) {
    ESP_LOGI(TAG, "tg_gw_s3 init — HTTPS dispatcher for telegram bot api");
    s_cfg_mutex = xSemaphoreCreateMutex();
    configASSERT(s_cfg_mutex);
    s_tg_q = xQueueCreate(kTgQueueDepth, sizeof(HapTgSend*));
    configASSERT(s_tg_q);
    BaseType_t ok = xTaskCreatePinnedToCore(tg_worker_task, "TgWorker",
                                zhac::stack::kTgWorker, nullptr, 4, nullptr, 0);
    configASSERT(ok == pdPASS);
}

// ── NVS helpers ──────────────────────────────────────────────────────────
static bool nvs_read_str(const char* key, char* out, size_t cap) {
    if (cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = cap;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK;
}

static bool nvs_write_str(const char* key, const char* val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// ── HAP-rx handlers (called from hap_bridge.cpp dispatch) ─────────────────
extern "C" void tg_gw_handle_settoken(const uint8_t* buf, uint16_t len) {
    HapTgSettoken m{};
    if (!hap_unpack_tg_settoken(buf, len, &m)) {
        ESP_LOGW(TAG, "TG_SETTOKEN unpack failed len=%u", len);
        return;
    }
    if (m.token_len > TG_TOKEN_MAX) {   // F45: reject over-long rather than truncate
        ESP_LOGW(TAG, "TG_SETTOKEN rejected — token len %u > max %u",
                 m.token_len, (unsigned)TG_TOKEN_MAX);
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool ok = nvs_write_str(NVS_TOKEN, m.token);
    xSemaphoreGive(s_cfg_mutex);
    ESP_LOGI(TAG, "TG_SETTOKEN persisted len=%u %s",
             m.token_len, ok ? "OK" : "FAILED");
}

extern "C" void tg_gw_handle_setchat(const uint8_t* buf, uint16_t len) {
    HapTgSetchat m{};
    if (!hap_unpack_tg_setchat(buf, len, &m)) {
        ESP_LOGW(TAG, "TG_SETCHAT unpack failed len=%u", len);
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool ok = nvs_write_str(NVS_CHAT, m.chat);
    xSemaphoreGive(s_cfg_mutex);
    ESP_LOGI(TAG, "TG_SETCHAT persisted chat=%s %s",
             m.chat, ok ? "OK" : "FAILED");
}

// JSON-escape into out, capped. Returns the bytes written (excl. trailing NUL).
// Escapes ", \\, control bytes, \\n, \\r, \\t. Truncates cleanly on overflow.
static size_t json_escape(const char* in, char* out, size_t cap) {
    if (!in || !out || cap == 0) return 0;
    size_t wi = 0;
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
        if (wi + n + 1 > cap) break;
        memcpy(out + wi, esc, n); wi += n;
    }
    out[wi] = '\0';
    return wi;
}

extern "C" void tg_gw_handle_send(const uint8_t* buf, uint16_t len) {
    if (!s_tg_q) {
        ESP_LOGE(TAG, "TG_SEND: worker not initialised");
        return;
    }
    auto* m = (HapTgSend*)calloc(1, sizeof(HapTgSend));
    if (!m) {
        ESP_LOGE(TAG, "TG_SEND: heap alloc failed (%u B)", (unsigned)sizeof(HapTgSend));
        return;
    }
    if (!hap_unpack_tg_send(buf, len, m)) {
        ESP_LOGW(TAG, "TG_SEND unpack failed len=%u", len);
        free(m);
        return;
    }
    if (xQueueSend(s_tg_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TG_SEND: queue full (depth=%u) — drop", (unsigned)kTgQueueDepth);
        free(m);
    }
}

static void tg_worker_task(void*) {
    for (;;) {
        HapTgSend* m = nullptr;
        if (xQueueReceive(s_tg_q, &m, portMAX_DELAY) != pdTRUE) continue;
        tg_perform_send(*m);
        free(m);
    }
}

static void tg_perform_send(const HapTgSend& m) {
    char token[TG_TOKEN_MAX + 1];   // F45: sized to the documented token max
    char default_chat[40];
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool have_token = nvs_read_str(NVS_TOKEN, token, sizeof(token));
    bool have_chat  = nvs_read_str(NVS_CHAT,  default_chat, sizeof(default_chat));
    xSemaphoreGive(s_cfg_mutex);
    if (!have_token || token[0] == '\0') {
        ESP_LOGW(TAG, "TG_SEND: token not set — drop");
        return;
    }
    const char* chat = m.chat_len ? m.chat : (have_chat ? default_chat : "");
    if (!chat[0]) {
        ESP_LOGW(TAG, "TG_SEND: no chat (default unset, no override) — drop");
        return;
    }

    // Build URL: https://api.telegram.org/bot<TOKEN>/sendMessage
    char url[200];
    int n = snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGE(TAG, "TG_SEND: url overflow (token len?)");
        return;
    }

    // Build body: {"chat_id":"...","text":"...","parse_mode":"..."}
    static char body[3500];
    static char text_esc[3200];
    json_escape(m.text, text_esc, sizeof(text_esc));
    int bn = 0;
    if (m.parse_mode_len) {
        bn = snprintf(body, sizeof(body),
                      "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"%s\"}",
                      chat, text_esc, m.parse_mode);
    } else {
        bn = snprintf(body, sizeof(body),
                      "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
                      chat, text_esc);
    }
    if (bn <= 0 || (size_t)bn >= sizeof(body)) {
        ESP_LOGE(TAG, "TG_SEND: body overflow (text too long after escape?)");
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url            = url;
    cfg.method         = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms     = 10000;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size    = 1024;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { ESP_LOGE(TAG, "TG_SEND: http_client_init failed"); return; }
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_post_field(cl, body, bn);

    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    int rxlen  = (int)esp_http_client_get_content_length(cl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TG_SEND: perform failed err=%s status=%d", esp_err_to_name(err), status);
    } else if (status / 100 != 2) {
        ESP_LOGW(TAG, "TG_SEND: HTTP %d (rxlen=%d, chat=%s, text_len=%u)",
                 status, rxlen, chat, m.text_len);
    } else {
        ESP_LOGI(TAG, "TG_SEND: HTTP %d OK chat=%s text_len=%u",
                 status, chat, m.text_len);
    }
    esp_http_client_cleanup(cl);
}

// Local-call stubs — S3 doesn't use these in MVP (all 3 verbs arrive over HAP).
bool tg_gw_settoken(const char*)              { return false; }
bool tg_gw_setchat(const char*)               { return false; }
bool tg_gw_send(const char*, const char*, const char*) { return false; }
