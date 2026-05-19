// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mqtt_gw.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "task_stacks.h"
#include "metrics/metrics_macros.h"

static const char* TAG = "mqtt_gw_s3";

static esp_mqtt_client_handle_t s_client     = nullptr;
static mqtt_rx_cb_t             s_rx_cb      = nullptr;
static bool                     s_connected  = false;
static char   s_sub_filter[128]              = {};
static int    s_sub_qos                      = 0;
static char   s_broker_url[128]              = {};
static char   s_client_id[32]                = {};   // empty → auto from MAC
static char   s_root_topic[32]               = "zhac";
static uint8_t s_fail_count                  = 0;
// Tolerate transient broker flaps / WiFi blips before giving up. The
// old cap of 3 was aggressive enough that a single reconnect storm
// right after boot (WiFi up → STA_GOT_IP delay → mqtt tries early)
// would destroy the client forever. 20 failures ≫ any realistic burst.
static constexpr uint8_t MQTT_MAX_FAILS      = 20;
static bool   s_enabled                      = false;  // user intent ("enabled" setting)

// Compose a default client id from the chip's base MAC if the operator
// never configured one. Stable across reboots; no name collision
// between two units on the same broker.
static void ensure_default_client_id() {
    if (s_client_id[0]) return;
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_client_id, sizeof(s_client_id), "zhac-%02x%02x",
                 mac[4], mac[5]);
    } else {
        strncpy(s_client_id, "zhac", sizeof(s_client_id) - 1);
    }
}

// (Re-)start the MQTT client with current broker URL + client id.
// Called from init + set_broker_url + set_client_id.
static void restart_client() {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    // F-09: refuse to start the client without an explicit broker URL.
    // The Kconfig fallback used to silently connect to whatever URL the
    // developer baked into local sdkconfig — supply-chain hazard.
    if (s_broker_url[0] == '\0') {
        ESP_LOGI(TAG, "mqtt restart skipped — no broker URL configured");
        return;
    }
    ensure_default_client_id();
    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri           = s_broker_url;
    cfg.credentials.client_id        = s_client_id;
    // Bump esp-mqtt's internal task stack from the 6 KB default. Two
    // problem paths trip it:
    //   - reconnect/TLS-error chain when the broker drops us repeatedly
    //   - publishing a multi-KB HAP frame body (DEVICE_JOIN with full
    //     ZapDevice JSON, big BULK_STATE_UPDATE batches)
    // Both deepen mqtt_task's call stack past the canary on 6 KB.
    cfg.task.stack_size              = 8192;
    // Lift the input/output buffer above the 1 KB default so single
    // big publishes (HAP_MAX_PAYLOAD = 4096 plus topic + protocol
    // overhead) no longer need fragmentation. Without this, large
    // DEVICE_JOIN / DEVICE_LEAVE / ALERT payloads silently truncate
    // or fragment, which interacts badly with brokers that reject
    // partial frames.
    cfg.buffer.size                  = 4608;
    cfg.buffer.out_size              = 4608;
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "mqtt init failed"); return; }
    extern void mqtt_event_handler(void*, esp_event_base_t, int32_t, void*);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                   mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "mqtt client restarted broker=%s client_id=%s",
             cfg.broker.address.uri, s_client_id);
}

// Forward decls for the disable-client deferred path. Full publisher
// queue + worker live below; the event handler only needs the type
// and the queue handle so it can post a sentinel item from MQTT-task
// context (esp-mqtt forbids stop/destroy from its own task).
struct PubItem {
    char*   topic;
    char*   payload;
    size_t  payload_len;
    uint8_t qos;
    bool    retain;
    bool    disable_client;
};
static constexpr int MQTT_PUBQ_DEPTH = 16;
static QueueHandle_t s_pubq = nullptr;

void mqtt_event_handler(void* arg, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t ev = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_fail_count = 0;
            ESP_LOGI(TAG, "MQTT connected");
            if (s_sub_filter[0]) {
                esp_mqtt_client_subscribe(s_client, s_sub_filter, s_sub_qos);
                ESP_LOGI(TAG, "MQTT subscribed to %s qos=%d", s_sub_filter, s_sub_qos);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            s_fail_count++;
            ESP_LOGW(TAG, "MQTT disconnected (fail %u/%u)", s_fail_count, MQTT_MAX_FAILS);
            if (s_fail_count >= MQTT_MAX_FAILS && s_client && s_pubq) {
                ESP_LOGE(TAG, "MQTT auto-disabled after %u consecutive failures", s_fail_count);
                // Defer stop+destroy to mqtt_pub worker. esp-mqtt forbids
                // calling stop from its own task — doing so trips the
                // recursive-mutex assert (spinlock count==0). The worker
                // owns a different task context and is safe.
                PubItem* sentinel = static_cast<PubItem*>(calloc(1, sizeof(PubItem)));
                if (sentinel) {
                    sentinel->disable_client = true;
                    if (xQueueSend(s_pubq, &sentinel, 0) != pdTRUE) {
                        free(sentinel);
                    }
                }
                // Don't reset s_fail_count — the worker will clear s_client
                // and a future mqtt_gw_start() resets the counter.
            }
            break;
        case MQTT_EVENT_DATA:
            if (s_rx_cb && ev->topic_len > 0) {
                s_rx_cb(ev->topic, ev->topic_len, ev->data, ev->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error type=%d",
                     ev->error_handle ? ev->error_handle->error_type : -1);
            break;
        default:
            break;
    }
}

// ── Non-blocking publish path ───────────────────────────────────────────
//
// mqtt_gw_publish() is called from many contexts, including the
// log-pipeline vprintf hook (log_ring → MQTT forwarding). If that path
// ever blocks — waiting on the MQTT outbox, a TCP retransmit, a broker
// stall — every task that emits an ESP_LOGx blocks behind the log
// mutex. That cascades into a full-system freeze: heartbeats, HTTP
// handlers, everything stops emitting logs and stops running.
//
// Fix: offload the actual esp_mqtt_client_publish call to a dedicated
// worker task draining a bounded queue. Producers never block — they
// heap-alloc a PubItem, xQueueSend with zero timeout, and drop on full.
// The worker is the only task that can stall on MQTT, and the stall is
// isolated to it.

static void mqtt_pub_worker(void*) {
    for (;;) {
        PubItem* item = nullptr;
        if (xQueueReceive(s_pubq, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!item) continue;

        if (item->disable_client) {
            // Deferred stop+destroy. Safe here because we're not in the
            // MQTT task. Clear s_client first so racing publishers skip
            // the now-doomed handle.
            esp_mqtt_client_handle_t doomed = s_client;
            s_client = nullptr;
            if (doomed) {
                esp_mqtt_client_stop(doomed);
                esp_mqtt_client_destroy(doomed);
            }
            free(item);
            continue;
        }

        if (s_client) {
            esp_mqtt_client_publish(s_client, item->topic, item->payload,
                                      (int)item->payload_len,
                                      (int)item->qos,
                                      item->retain ? 1 : 0);
        }
        free(item->topic);
        free(item->payload);
        free(item);
    }
}

void mqtt_gw_init() {
    if (!s_pubq) {
        s_pubq = xQueueCreate(MQTT_PUBQ_DEPTH, sizeof(PubItem*));
        if (s_pubq) {
            xTaskCreate(mqtt_pub_worker, "mqtt_pub", zhac::stack::kMqttPubS3, nullptr,
                         tskIDLE_PRIORITY + 3, nullptr);
        }
    }
    // Do NOT start the MQTT client here. mqtt_gw_init runs unconditionally
    // at boot; if the user has MQTT disabled in NVS (enabled=0) and we
    // started a client anyway, esp-mqtt's reconnect loop would bang on the
    // network forever even though main.cpp's disabled branch never calls
    // configure(). The client now starts only via mqtt_gw_configure() →
    // set_broker_url() → restart_client(), or via mqtt_gw_start() from
    // api_settings_set when the user re-enables.
}

void mqtt_gw_start() {
    if (s_client) return;        // already up
    s_fail_count = 0;            // clear auto-disable trip
    restart_client();
}

void mqtt_gw_stop() {
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client      = nullptr;
    s_connected   = false;
    s_fail_count  = 0;
    s_enabled     = false;
    ESP_LOGI(TAG, "mqtt client stopped");
}

void mqtt_gw_configure(const char* url, const char* root, const char* cid) {
    if (url && url[0]) {
        strncpy(s_broker_url, url, sizeof(s_broker_url) - 1);
        s_broker_url[sizeof(s_broker_url) - 1] = '\0';
    }
    if (root && root[0]) mqtt_gw_set_root_topic(root);
    if (cid  && cid[0])  {
        strncpy(s_client_id, cid, sizeof(s_client_id) - 1);
        s_client_id[sizeof(s_client_id) - 1] = '\0';
    }
    s_enabled = (s_broker_url[0] != '\0');
}

static void deferred_mqtt_start_cb(void*) {
    if (!s_enabled || !s_broker_url[0] || s_client) return;
    ESP_LOGI(TAG, "deferred MQTT start broker=%s", s_broker_url);
    s_fail_count = 0;
    restart_client();
}

void mqtt_gw_on_sta_up() {
    // STA got an IP, but the kernel network stack (route table, ARP
    // cache) isn't fully ready in the same instant — connecting to
    // the broker right now produces a "Connection reset by peer"
    // because the SYN goes out before everything's settled, then
    // esp-mqtt's 10-s reconnect loop bangs against it for minutes.
    // Defer the first connect attempt by 5 s; that's empirically
    // enough headroom on a typical SOHO LAN. esp-mqtt handles its own
    // reconnect backoff after the first successful start.
    //
    // Note: mqtt_gw_init / mqtt_gw_configure run before WiFi has an
    // IP and may already have started a doomed client. We tear it
    // down first so the deferred restart starts cleanly. Without this
    // the s_client guard would early-return and the defer never fires
    // — esp-mqtt's internal retry loop would then own the lifecycle
    // and could fail for minutes against a broker that sees malformed
    // first packets.
    if (!s_enabled || !s_broker_url[0]) return;
    if (s_client) {
        ESP_LOGI(TAG, "STA up — tearing down premature client before defer");
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    ESP_LOGI(TAG, "STA up — scheduling MQTT start in 5s broker=%s", s_broker_url);
    static esp_timer_handle_t s_start_timer = nullptr;
    if (!s_start_timer) {
        const esp_timer_create_args_t cfg = {
            .callback = &deferred_mqtt_start_cb,
            .arg      = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name     = "mqtt_start",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&cfg, &s_start_timer);
    }
    esp_timer_stop(s_start_timer);
    esp_timer_start_once(s_start_timer, 5 * 1000 * 1000ULL);
}

void mqtt_gw_set_broker_url(const char* url) {
    if (!url || !url[0]) return;
    strncpy(s_broker_url, url, sizeof(s_broker_url) - 1);
    s_broker_url[sizeof(s_broker_url) - 1] = '\0';
    restart_client();
}

void mqtt_gw_set_client_id(const char* id) {
    if (!id || !id[0]) return;
    strncpy(s_client_id, id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';
    if (s_client) restart_client();   // applies live on running client
}

void mqtt_gw_set_root_topic(const char* root) {
    if (!root || !root[0]) { s_root_topic[0] = '\0'; return; }
    strncpy(s_root_topic, root, sizeof(s_root_topic) - 1);
    s_root_topic[sizeof(s_root_topic) - 1] = '\0';
    // Trim trailing slash if present — mqtt_gw_format_topic always
    // inserts one.
    size_t n = strlen(s_root_topic);
    if (n && s_root_topic[n - 1] == '/') s_root_topic[n - 1] = '\0';
}

const char* mqtt_gw_get_root_topic(void) {
    return s_root_topic[0] ? s_root_topic : "zhac";
}

int mqtt_gw_format_topic(char* out, size_t cap, const char* suffix) {
    if (!out || cap == 0) return -1;
    const char* root = s_root_topic[0] ? s_root_topic : "zhac";
    const char* sfx  = suffix ? suffix : "";
    if (sfx[0] == '/') sfx++;   // avoid `<root>//suffix`
    int n = snprintf(out, cap, "%s/%s", root, sfx);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

bool mqtt_gw_is_connected() {
    return s_connected;
}

bool mqtt_gw_is_active() {
    return s_client != nullptr;
}

void mqtt_gw_set_rx_callback(mqtt_rx_cb_t cb) {
    s_rx_cb = cb;
}

void mqtt_gw_subscribe(const char* topic_filter, int qos) {
    if (!topic_filter) return;
    strncpy(s_sub_filter, topic_filter, sizeof(s_sub_filter) - 1);
    s_sub_filter[sizeof(s_sub_filter) - 1] = '\0';
    s_sub_qos = qos;
    // If already connected, subscribe immediately
    if (s_client) {
        esp_mqtt_client_subscribe(s_client, s_sub_filter, s_sub_qos);
        ESP_LOGI(TAG, "MQTT subscribed to %s qos=%d", s_sub_filter, s_sub_qos);
    }
}

void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                      int qos, bool retain) {
    if (!s_client || !s_pubq || !topic || !payload) return;

    // Topic prefixing rules:
    //   - Leading '/' → absolute (skip root prefix; strip leading '/').
    //   - Already starts with "<root>/" → use as-is (system publishers
    //     pre-format via mqtt_gw_format_topic; don't double-prefix).
    //   - Otherwise → prepend "<root>/" so DSL `publish foo bar` and Lua
    //     `zhac.publish("foo", ...)` land under the configured root.
    char fmt_topic[160];
    const char* eff = topic;
    if (topic[0] == '/') {
        eff = topic + 1;     // absolute — strip the slash, no root
    } else if (s_root_topic[0]) {
        const size_t rlen = strlen(s_root_topic);
        if (strncmp(topic, s_root_topic, rlen) == 0 && topic[rlen] == '/') {
            eff = topic;     // already root-prefixed
        } else {
            int n = snprintf(fmt_topic, sizeof(fmt_topic), "%s/%s",
                              s_root_topic, topic);
            if (n > 0 && (size_t)n < sizeof(fmt_topic)) eff = fmt_topic;
        }
    }

    // Copy topic + payload into a heap-allocated item so the caller can
    // reuse/discard its buffers immediately. Drop silently on alloc
    // failure or queue full — no logging here, otherwise the log-path
    // caller would recurse into us (or spam during broker stalls).
    PubItem* item = (PubItem*)calloc(1, sizeof(PubItem));
    if (!item) return;
    item->topic        = strdup(eff);
    item->payload_len  = payload_len;
    item->payload      = (char*)malloc(payload_len + 1);
    item->qos          = (uint8_t)qos;
    item->retain       = retain;
    item->disable_client = false;
    if (!item->topic || !item->payload) {
        if (item->topic)   free(item->topic);
        if (item->payload) free(item->payload);
        free(item);
        return;
    }
    memcpy(item->payload, payload, payload_len);
    item->payload[payload_len] = '\0';

    if (xQueueSend(s_pubq, &item, 0) != pdTRUE) {
        // F-07: surface drops so operators can detect broker stalls or
        // log-storm saturation. Still no ESP_LOG here — log-pipeline
        // recursion is the original reason for the silent drop.
        _METRIC_COUNTER_INC(METRIC_MQTT_DROPPED_MSGS, 1);
        free(item->topic);
        free(item->payload);
        free(item);
    }
}
