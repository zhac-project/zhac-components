// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Telegram from a single-chip hub (zhac-wired-core: ESP32-P4, ESP32-S31): the
// hub posts to api.telegram.org itself. Lua's zhac.telegram_* land here.
//
// The bot token and default chat live in NVS (zhac/tg_token, zhac/tg_chat, the
// keys the two-chip S3 used) and in RAM. tg_gw_send() builds the body in the
// caller's task, so a bad chat or parse_mode comes back as false, then queues
// it for one worker task that does the HTTPS POST. The worker (TLS needs a big
// stack) is started on the first send: a hub that never sends pays nothing.
#include "tg_gw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "task_stacks.h"
#include "tg_gw_body.h"
#include "zap_tls.h"
#include "zhac_task.h"

namespace {

constexpr const char* TAG = "tg_gw";
constexpr const char* kNs = "zhac";
constexpr const char* kKeyToken = "tg_token";
constexpr const char* kKeyChat = "tg_chat";
constexpr size_t kBodyMax = 3500;
constexpr int kRetries = 20;          // a send that got no answer, 429 or 5xx: 10 minutes in all
constexpr uint32_t kRetryMs = 30000;
constexpr UBaseType_t kQueueDepth = 4;

struct Job {
    int len;
    char body[kBodyMax];
};

SemaphoreHandle_t s_mu;   // s_token, s_chat, s_q
char s_token[TG_TOKEN_MAX + 1];
char s_chat[TG_CHAT_MAX + 1];
QueueHandle_t s_q;

void nvs_load(const char* key, char* out, size_t cap) {
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
    nvs_close(h);
}

bool nvs_save(const char* key, const char* val) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

const char* why(int status) {
    switch (status) {
        case 400: return "Telegram refused the message (chat not found, or bad text for parse_mode)";
        case 401: return "Telegram refused the bot token";
        case 403: return "the bot may not write to this chat (send it a message first, or add it to the group)";
        case 429: return "too many messages: Telegram asks to slow down";
        default:  return "Telegram error";
    }
}

// True when the send is worth trying again (tg_send_retry).
bool post(const Job& j) {
    char url[48 + TG_TOKEN_MAX];
    xSemaphoreTake(s_mu, portMAX_DELAY);
    std::snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_token);
    xSemaphoreGive(s_mu);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = zap_tls_attach;
    cfg.timeout_ms = 10000;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { ESP_LOGW(TAG, "send: no memory for the HTTPS client"); return false; }
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_post_field(cl, j.body, j.len);
    const esp_err_t e = esp_http_client_perform(cl);
    const int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    if (e != ESP_OK) ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(e));
    else if (status / 100 != 2) ESP_LOGW(TAG, "send: HTTP %d, %s", status, why(status));
    else ESP_LOGI(TAG, "sent (%d B)", j.len);
    return status / 100 != 2 && tg_send_retry(e == ESP_OK, status);
}

void worker(void*) {
    for (;;) {
        Job* j = nullptr;
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        // A message the network could not take yet (no link, a router still booting after a power
        // cut) is tried again every 30 s for 10 minutes; later messages wait behind it.
        for (int i = 0; post(*j) && i < kRetries; i++) vTaskDelay(pdMS_TO_TICKS(kRetryMs));
        heap_caps_free(j);
    }
}

// Under s_mu. The queue and the worker, made on the first send.
bool start_worker_locked() {
    if (s_q) return true;
    QueueHandle_t q = zhac_queue_create(kQueueDepth, sizeof(Job*));
    if (!q) return false;
    s_q = q;
    if (zhac_task_create(worker, "TgWorker", zhac::stack::kTgWorker, nullptr, 2, nullptr) != pdPASS) {
        s_q = nullptr;
        zhac_queue_delete(q);
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

void tg_gw_init(void) {
    s_mu = xSemaphoreCreateMutex();
    nvs_load(kKeyToken, s_token, sizeof(s_token));
    nvs_load(kKeyChat, s_chat, sizeof(s_chat));
    if (!tg_token_ok(s_token)) s_token[0] = '\0';   // it goes into the URL: only what settoken takes
    if (!tg_chat_ok(s_chat)) s_chat[0] = '\0';
    ESP_LOGI(TAG, "Telegram: %s, %s", s_token[0] ? "bot token set" : "no bot token",
             s_chat[0] ? "default chat set" : "no default chat");
}

bool tg_gw_settoken(const char* token) {
    if (!s_mu) return false;
    if (!tg_token_ok(token)) {
        ESP_LOGW(TAG, "settoken: not a bot token (<digits>:<secret>, at most %u chars)", (unsigned)TG_TOKEN_MAX);
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = nvs_save(kKeyToken, token);
    if (ok) std::strcpy(s_token, token);
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "settoken: %s", ok ? "saved" : "could not save");
    return ok;
}

bool tg_gw_setchat(const char* chat) {
    if (!s_mu) return false;
    if (!tg_chat_ok(chat)) {
        ESP_LOGW(TAG, "setchat: not a chat id or @username");
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = nvs_save(kKeyChat, chat);
    if (ok) std::strcpy(s_chat, chat);
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "setchat: %s %s", chat, ok ? "saved" : "could not save");
    return ok;
}

bool tg_gw_send(const char* text, const char* chat_or_null, const char* parse_mode_or_null) {
    if (!s_mu || !text) return false;
    char def_chat[TG_CHAT_MAX + 1];
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool have_token = s_token[0] != '\0';
    std::strcpy(def_chat, s_chat);
    const bool running = have_token && start_worker_locked();
    xSemaphoreGive(s_mu);
    // Not copied: a too-long override must fail the check whole, not pass it cut short.
    const char* chat = chat_or_null ? chat_or_null : def_chat;
    if (!have_token) { ESP_LOGW(TAG, "send: no bot token, set one with zhac.telegram_settoken"); return false; }
    if (!chat[0]) { ESP_LOGW(TAG, "send: no chat, pass one or set it with zhac.telegram_setchat"); return false; }
    if (!running) { ESP_LOGW(TAG, "send: could not start the sender task (memory)"); return false; }

    auto* j = static_cast<Job*>(heap_caps_malloc(sizeof(Job), MALLOC_CAP_SPIRAM));
    if (!j) j = static_cast<Job*>(malloc(sizeof(Job)));
    if (!j) { ESP_LOGW(TAG, "send: out of memory"); return false; }
    j->len = tg_build_body(text, chat, parse_mode_or_null, j->body, sizeof(j->body));
    if (j->len < 0) {
        ESP_LOGW(TAG, "send: refused, bad chat or parse_mode (Markdown, MarkdownV2 or HTML)");
        heap_caps_free(j);
        return false;
    }
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        ESP_LOGW(TAG, "send: %u messages already waiting, dropped", (unsigned)kQueueDepth);
        heap_caps_free(j);
        return false;
    }
    return true;
}

}  // extern "C"
