// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mqtt_gw.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"   // F10: verify broker cert for mqtts://
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "task_stacks.h"
#include "metrics/metrics_macros.h"
#include "zap_store.h"   // RainMaker bridge Phase 2: zhac_uplink_get()/ZHAC_UPLINK_*

static const char* TAG = "mqtt_gw_s3";

// F7-T19: ONE mutex owns the entire esp_mqtt_client_* lifecycle —
// init/start/stop/destroy/reconnect/subscribe/unsubscribe — AND the
// publish call in the worker. Rationale: s_client is torn down and
// recreated from many task contexts (REST settings handlers, the
// esp_timer re-arm/deferred-start callbacks, the STA-up handler, the
// pub-worker's deferred disable) while the worker may be mid-publish.
// esp_mqtt_client_publish is thread-safe against a LIVE handle, but
// esp_mqtt_client_destroy is NOT safe concurrent with any use → a
// destroy racing a publish is a use-after-free of the client handle.
//
// The worker is the ONLY task that publishes (all producers funnel
// through s_pubq → single worker), so publishes are already serialized
// by the queue; the mutex's real job is to fence destroy/recreate vs
// the in-flight publish + handle reads. We publish UNDER the lock
// (see mqtt_pub_worker): esp_mqtt_client_publish can block for QoS>0
// (it waits on the outbox under esp-mqtt's own API lock), so the only
// task that can stall behind a long publish is a concurrent lifecycle
// op — i.e. a rare REST config write — which is acceptable for a
// single-user gateway and is far simpler / less bug-prone than a
// snapshot-and-revalidate handshake (which would reintroduce a
// destroy-vs-use window). restart_client_locked() below assumes the
// caller ALREADY holds s_client_mtx; every callsite takes the mutex
// explicitly around it (there is no separate locking wrapper).
static SemaphoreHandle_t        s_client_mtx = nullptr;

static esp_mqtt_client_handle_t s_client     = nullptr;
static mqtt_rx_cb_t             s_rx_cb      = nullptr;
static bool                     s_connected  = false;
static char   s_sub_filter[128]              = {};
static int    s_sub_qos                      = 0;
static char   s_broker_url[128]              = {};
static char   s_client_id[32]                = {};   // empty → auto from MAC
static char   s_root_topic[32]               = "zhac";
static uint8_t s_fail_count                  = 0;
static esp_timer_handle_t s_rearm_tmr        = nullptr;   // F40: auto-disable cooldown re-arm
// Tolerate transient broker flaps / WiFi blips before giving up. The
// old cap of 3 was aggressive enough that a single reconnect storm
// right after boot (WiFi up → STA_GOT_IP delay → mqtt tries early)
// would destroy the client forever. 20 failures ≫ any realistic burst.
static constexpr uint8_t MQTT_MAX_FAILS      = 20;
static bool   s_enabled                      = false;  // user intent ("enabled" setting)

// F4-T19: the broker URL is the only carrier of credentials
// (mqtt://user:pass@host). Logging it verbatim leaks creds into the
// log pipeline (and onward to MQTT log-forwarding). Print scheme +
// host:port only, dropping any `userinfo` between "://" and the "@".
// Returns a pointer to a static thread-local-ish buffer; callers use
// it inline in a single ESP_LOGx and don't retain it. Only ever
// called from lifecycle paths already serialized by s_client_mtx, so
// the shared static buffer is safe.
static const char* redact_userinfo(const char* url) {
    static char buf[128];
    if (!url || !url[0]) { buf[0] = '\0'; return buf; }
    const char* p = strstr(url, "://");
    if (!p) {
        strncpy(buf, url, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
    const char* scheme_end = p + 3;          // points at first authority char
    // Userinfo (if any) runs from scheme_end up to the LAST '@' before
    // the first '/' of the path (a password may legitimately contain '@'
    // only when percent-encoded, but be conservative: cut at the last '@'
    // within the authority).
    const char* path = strchr(scheme_end, '/');
    const char* at   = nullptr;
    for (const char* q = scheme_end; *q && (!path || q < path); ++q) {
        if (*q == '@') at = q;
    }
    const char* host = at ? at + 1 : scheme_end;
    int n = snprintf(buf, sizeof(buf), "%.*s%s",
                     (int)(scheme_end - url), url, host);
    if (n < 0) buf[0] = '\0';
    return buf;
}

// F3-T19: validate a publish topic name (NOT a subscription filter).
// MQTT 3.1.1 §4.7: a PUBLISH topic name MUST NOT contain wildcards
// ('+' / '#'); a spec-compliant broker drops the connection on a
// wildcard PUBLISH, so one bad rule/Lua topic = reconnect churn for
// the whole gateway. Also reject NUL/control bytes (U+0000 is outright
// forbidden; control chars are ill-advised) and overlong names. The
// cap matches the fmt_topic staging buffer in mqtt_gw_publish.
static bool mqtt_topic_ok(const char* t) {
    if (!t || !t[0]) return false;
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)t; *p; ++p, ++n) {
        if (n >= 159) return false;            // overlong (fmt_topic is 160)
        if (*p == '+' || *p == '#') return false;
        if (*p < 0x20 || *p == 0x7f) return false;  // control / DEL
    }
    return n > 0;
}

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
// Assumes the caller already holds s_client_mtx — every callsite takes
// the mutex itself around this call (there is no locking wrapper).
static void restart_client_locked() {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    // RainMaker bridge Phase 2: this is the SOLE place that ever calls
    // esp_mqtt_client_init/start (every lifecycle entry — mqtt_gw_start,
    // the STA-up deferred start, the auto-disable cooldown re-arm,
    // set_broker_url, set_client_id — routes through here, same reasoning
    // as the "no separate locking wrapper" note above). Gating it here is
    // therefore equivalent to gating every start path individually, without
    // duplicating the check. An absent NVS key (pre-selector devices)
    // resolves to ZHAC_UPLINK_CUSTOM_MQTT in zap_store, so this is a no-op
    // for default/existing configurations. The teardown above still runs
    // first so a runtime selector flip away from custom_mqtt — observed by
    // a stale caller re-entering here before mqtt_gw_stop() is invoked —
    // can't leave a stray client running.
    if (zhac_uplink_get() != ZHAC_UPLINK_CUSTOM_MQTT) {
        ESP_LOGI(TAG, "uplink != custom_mqtt — mqtt_gw disabled");
        return;
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
    // F10 (FINDINGS.md): for a TLS broker (mqtts:// / wss://) verify the
    // server certificate chain against the bundled CA store. Without this an
    // mqtts:// connection is unauthenticated and a MITM can impersonate the
    // broker. Plain mqtt:// (no TLS) is unaffected.
    if (strncmp(s_broker_url, "mqtts://", 8) == 0 ||
        strncmp(s_broker_url, "wss://",   6) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
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
    // F45 (FINDINGS.md): Last-Will so the broker announces an ungraceful drop.
    // We publish retained "online" on connect (below); if the TCP/keepalive
    // dies the broker publishes this retained "offline" to the same topic, so
    // subscribers get a real availability signal. will_topic stays in scope
    // through esp_mqtt_client_init, which copies the config strings.
    char will_topic[160];
    if (mqtt_gw_format_topic(will_topic, sizeof(will_topic), "availability") > 0) {
        cfg.session.last_will.topic   = will_topic;
        cfg.session.last_will.msg     = "offline";
        cfg.session.last_will.msg_len = 7;
        cfg.session.last_will.qos     = 1;
        cfg.session.last_will.retain  = 1;
    }
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "mqtt init failed"); return; }
    extern void mqtt_event_handler(void*, esp_event_base_t, int32_t, void*);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                   mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
    // F4-T19: redact credentials from the broker URL before logging.
    ESP_LOGI(TAG, "mqtt client restarted broker=%s client_id=%s",
             redact_userinfo(cfg.broker.address.uri), s_client_id);
}

// NB: every lifecycle entry point (mqtt_gw_start/stop, set_broker_url,
// set_client_id, the timer + STA callbacks, the pub-worker disable)
// takes s_client_mtx itself and calls restart_client_locked() directly,
// so there is no separate locking wrapper — the explicit take/give at
// each callsite keeps the lock scope visible alongside the state it
// guards (URL/client-id updates).

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
            // NB: this handler runs on esp-mqtt's internal mqtt_task, so it
            // does NOT take s_client_mtx before subscribe/publish below.
            // esp_mqtt_client_destroy stop-JOINS this task before freeing the
            // handle, so s_client stays alive for the whole handler — the
            // lock-free access is safe. Taking s_client_mtx here would instead
            // risk a lifecycle-op deadlock (esp-mqtt #163 class): a lifecycle
            // op holding the mutex while joining this task that is blocked on
            // the same mutex.
            s_connected = true;
            s_fail_count = 0;
            ESP_LOGI(TAG, "MQTT connected");
            if (s_sub_filter[0]) {
                esp_mqtt_client_subscribe(s_client, s_sub_filter, s_sub_qos);
                ESP_LOGI(TAG, "MQTT subscribed to %s qos=%d", s_sub_filter, s_sub_qos);
            }
            // F45: announce availability "online" (retained), pairing with the
            // LWT "offline" the broker holds for our ungraceful disconnect.
            {
                char av[160];
                if (mqtt_gw_format_topic(av, sizeof(av), "availability") > 0)
                    esp_mqtt_client_publish(s_client, av, "online", 6, 1, 1);
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
            // F2-T19: handle fragmentation. esp-mqtt splits an inbound
            // payload larger than cfg.buffer.size (4608 B) across
            // multiple MQTT_EVENT_DATA events: the FIRST carries the
            // topic (topic_len>0) and data_len==first chunk, with
            // total_data_len==full size; CONTINUATION chunks have
            // topic_len==0. Forwarding the first chunk to rx_cb would
            // silently truncate the command with no indication.
            //
            // The inbound command contract is small (well under one
            // buffer), so reassembly is unnecessary — instead DROP any
            // fragmented payload and warn once. Continuation chunks
            // (topic_len==0) are dropped implicitly by the topic_len>0
            // guard below.
            if (ev->topic_len > 0 && ev->data_len != ev->total_data_len) {
                static bool s_frag_warned = false;
                if (!s_frag_warned) {
                    s_frag_warned = true;
                    ESP_LOGW(TAG, "MQTT_EVENT_DATA: dropping fragmented "
                             "payload (%d of %d B) — inbound commands must "
                             "fit one buffer (this warning logs once)",
                             ev->data_len, ev->total_data_len);
                }
                break;
            }
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

// F40 (FINDINGS.md): after the auto-disable trip, re-arm the client once a
// cooldown elapses so a transient broker outage (>MQTT_MAX_FAILS) recovers
// without a manual restart or WiFi reconnect. Runs on the esp_timer task.
static void mqtt_rearm_cb(void*) {
    s_fail_count = 0;
    // F1-T19: lock the whole check-then-restart so it can't race a
    // concurrent stop/destroy/recreate. restart_client_locked() runs
    // under the mutex we hold here, which also covers the !s_client test.
    if (!s_client_mtx) return;
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    if (s_enabled && s_broker_url[0] && !s_client) {
        ESP_LOGI(TAG, "MQTT re-arm after cooldown");
        restart_client_locked();
    }
    xSemaphoreGive(s_client_mtx);
}

static void mqtt_pub_worker(void*) {
    for (;;) {
        PubItem* item = nullptr;
        if (xQueueReceive(s_pubq, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!item) continue;

        if (item->disable_client) {
            // Deferred stop+destroy. Safe here because we're not in the
            // MQTT event task. F1-T19: take s_client_mtx so the destroy
            // can't race a lifecycle op (restart/set_broker_url) on
            // another task. We're the only publisher and we publish
            // under the same lock, so there's no in-flight publish to
            // wait on here — but the lock still fences a concurrent
            // restart_client_locked() that might be mid esp_mqtt_client_init.
            if (s_client_mtx) xSemaphoreTake(s_client_mtx, portMAX_DELAY);
            esp_mqtt_client_handle_t doomed = s_client;
            s_client = nullptr;
            if (doomed) {
                esp_mqtt_client_stop(doomed);
                esp_mqtt_client_destroy(doomed);
            }
            if (s_client_mtx) xSemaphoreGive(s_client_mtx);
            free(item);
            // F40: schedule a cooldown re-arm (5 min) instead of staying off
            // until a manual restart / WiFi reconnect.
            if (!s_rearm_tmr) {
                const esp_timer_create_args_t a = {
                    .callback = &mqtt_rearm_cb, .arg = nullptr,
                    .dispatch_method = ESP_TIMER_TASK, .name = "mqtt_rearm",
                    .skip_unhandled_events = true };
                esp_timer_create(&a, &s_rearm_tmr);
            }
            if (s_rearm_tmr) {
                esp_timer_stop(s_rearm_tmr);
                esp_timer_start_once(s_rearm_tmr, (uint64_t)300 * 1000000ULL);
            }
            continue;
        }

        // F1-T19: publish under s_client_mtx so the handle cannot be
        // destroyed by a lifecycle op on another task while we're inside
        // esp_mqtt_client_publish (use-after-free). Snapshot the handle
        // under the lock and use the snapshot. esp_mqtt_client_publish
        // may block for QoS>0, but the only task that can be stalled
        // behind us is a concurrent (rare) lifecycle op — acceptable for
        // a gateway, and far simpler than a destroy-vs-use handshake.
        if (s_client_mtx) xSemaphoreTake(s_client_mtx, portMAX_DELAY);
        esp_mqtt_client_handle_t cl = s_client;
        if (cl) {
            esp_mqtt_client_publish(cl, item->topic, item->payload,
                                      (int)item->payload_len,
                                      (int)item->qos,
                                      item->retain ? 1 : 0);
        }
        if (s_client_mtx) xSemaphoreGive(s_client_mtx);
        free(item->topic);
        free(item->payload);
        free(item);
    }
}

void mqtt_gw_init() {
    // F1-T19: create the client lifecycle mutex during single-threaded
    // boot, before any task can call a lifecycle op. Lazy creation in
    // the first caller would itself be a race.
    if (!s_client_mtx) s_client_mtx = xSemaphoreCreateMutex();
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
    // set_broker_url() → restart_client_locked(), or via mqtt_gw_start()
    // from api_settings_set when the user re-enables.
}

void mqtt_gw_start() {
    if (!s_client_mtx) return;
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    if (s_client) {              // already up
        xSemaphoreGive(s_client_mtx);
        return;
    }
    s_fail_count = 0;            // clear auto-disable trip
    restart_client_locked();
    xSemaphoreGive(s_client_mtx);
}

void mqtt_gw_stop() {
    if (!s_client_mtx) return;
    // F1-T19: stop/destroy under the lock so it can't race a publish or
    // a concurrent restart.
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    if (!s_client) {
        s_enabled = false;
        xSemaphoreGive(s_client_mtx);
        return;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client      = nullptr;
    s_connected   = false;
    s_fail_count  = 0;
    s_enabled     = false;
    xSemaphoreGive(s_client_mtx);
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
    if (!s_client_mtx) return;
    // F1-T19: lock the check-then-restart; F4-T19: redact URL creds.
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    if (!s_enabled || !s_broker_url[0] || s_client) {
        xSemaphoreGive(s_client_mtx);
        return;
    }
    ESP_LOGI(TAG, "deferred MQTT start broker=%s", redact_userinfo(s_broker_url));
    s_fail_count = 0;
    restart_client_locked();
    xSemaphoreGive(s_client_mtx);
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
    // F1-T19: tear down the premature client under the lock; F4-T19:
    // redact URL creds.
    if (s_client_mtx) xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    if (s_client) {
        ESP_LOGI(TAG, "STA up — tearing down premature client before defer");
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    if (s_client_mtx) xSemaphoreGive(s_client_mtx);
    ESP_LOGI(TAG, "STA up — scheduling MQTT start in 5s broker=%s",
             redact_userinfo(s_broker_url));
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
    if (!url || !url[0] || !s_client_mtx) return;
    // F1-T19: hold the lock across the URL update + restart so the
    // restart reads a consistent URL and can't race another lifecycle op.
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    strncpy(s_broker_url, url, sizeof(s_broker_url) - 1);
    s_broker_url[sizeof(s_broker_url) - 1] = '\0';
    restart_client_locked();
    xSemaphoreGive(s_client_mtx);
}

void mqtt_gw_set_client_id(const char* id) {
    if (!id || !id[0] || !s_client_mtx) return;
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    strncpy(s_client_id, id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';
    if (s_client) restart_client_locked();   // applies live on running client
    xSemaphoreGive(s_client_mtx);
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

bool mqtt_gw_is_secure() {
    // F45 (FINDINGS.md): only a verified-TLS scheme counts as secure.
    return strncmp(s_broker_url, "mqtts://", 8) == 0 ||
           strncmp(s_broker_url, "wss://",   6) == 0;
}

void mqtt_gw_set_rx_callback(mqtt_rx_cb_t cb) {
    s_rx_cb = cb;
}

void mqtt_gw_subscribe(const char* topic_filter, int qos) {
    if (!topic_filter || !s_client_mtx) return;
    // F1-T19: take the lock around the live subscribe/unsubscribe so the
    // handle can't be destroyed under us.
    xSemaphoreTake(s_client_mtx, portMAX_DELAY);
    // F5-T19: we keep only ONE stored subscription filter. If a prior
    // filter is live, unsubscribe it before overwriting — otherwise the
    // broker keeps delivering the old filter's messages until the next
    // reconnect (the CONNECTED handler only (re)subscribes s_sub_filter).
    if (s_client && s_sub_filter[0] &&
        strcmp(s_sub_filter, topic_filter) != 0) {
        esp_mqtt_client_unsubscribe(s_client, s_sub_filter);
        ESP_LOGI(TAG, "MQTT unsubscribed prior filter %s", s_sub_filter);
    }
    strncpy(s_sub_filter, topic_filter, sizeof(s_sub_filter) - 1);
    s_sub_filter[sizeof(s_sub_filter) - 1] = '\0';
    s_sub_qos = qos;
    // If already connected, subscribe immediately
    if (s_client) {
        esp_mqtt_client_subscribe(s_client, s_sub_filter, s_sub_qos);
        ESP_LOGI(TAG, "MQTT subscribed to %s qos=%d", s_sub_filter, s_sub_qos);
    }
    xSemaphoreGive(s_client_mtx);
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
            if (n > 0 && (size_t)n < sizeof(fmt_topic)) {
                eff = fmt_topic;
            } else {
                // F3-T19: prefix-format overflow. The OLD code fell
                // through with eff==topic and silently published to the
                // *unprefixed* topic — a topic-confusion bug. DROP
                // instead: a rule whose root-prefixed name overflows is
                // misconfigured; publishing it bare is worse than not
                // publishing.
                _METRIC_COUNTER_INC(METRIC_MQTT_DROPPED_MSGS, 1);
                return;
            }
        }
    }

    // F3-T19: reject MQTT wildcards / control chars in the PUBLISH topic.
    // A wildcard ('+'/'#') in a PUBLISH topic name makes a spec-compliant
    // broker drop the connection (MQTT 3.1.1 §4.7) → one bad rule/Lua
    // topic = reconnect churn for the whole gateway. Validate the
    // effective (post-prefix) name and drop on failure.
    if (!mqtt_topic_ok(eff)) {
        _METRIC_COUNTER_INC(METRIC_MQTT_DROPPED_MSGS, 1);
        return;
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
    // F23 (FINDINGS.md): clamp QoS to the valid 0..2 range before esp-mqtt
    // (a rule/Lua publish could pass anything).
    item->qos          = (uint8_t)(qos < 0 ? 0 : (qos > 2 ? 2 : qos));
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
