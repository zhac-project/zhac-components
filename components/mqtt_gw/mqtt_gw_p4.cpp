// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mqtt_gw.h"
#include "hap_slave.h"
#include "hap_session.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

static const char* TAG = "mqtt_gw_p4";

void mqtt_gw_init() {
    ESP_LOGI(TAG, "mqtt_gw_p4 init — publishes routed via HAP to S3");
}

// P4 has no local client — start/stop/configure are handled on S3.
void mqtt_gw_start() {}
void mqtt_gw_stop()  {}
void mqtt_gw_configure(const char*, const char*, const char*) {}
void mqtt_gw_on_sta_up() {}

void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                      int qos, bool retain) {
    if (!topic || !payload) return;

    // Static buffers + mutex. Earlier we used `std::make_unique` to
    // move these off-stack (hap_slave low-stack canary trip), but
    // bursty PUBLISH actions (magic cube spam → many rules → many
    // publishes/sec) churned ~1.3 KB heap per call, fragmenting
    // internal RAM on P4 (no PSRAM in the hot path) until newlib
    // failed to allocate a recursive mutex inside fread → abort.
    // One-shot alloc, mutex-serialised, no churn.
    static SemaphoreHandle_t s_mutex = nullptr;
    static HapMqttPublish    s_msg{};
    constexpr size_t         kBufSz = 700;
    static uint8_t           s_buf[kBufSz];
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_publish: mutex contention — drop topic=%s", topic);
        return;
    }

    std::memset(&s_msg, 0, sizeof(s_msg));
    strncpy(s_msg.topic, topic, sizeof(s_msg.topic) - 1);
    // HapMqttPublish::payload is a fixed char[512]; cap + NUL-terminate.
    const size_t n = payload_len < sizeof(s_msg.payload) - 1
                       ? payload_len : sizeof(s_msg.payload) - 1;
    memcpy(s_msg.payload, payload, n);
    s_msg.payload[n] = '\0';
    s_msg.qos    = static_cast<uint8_t>(qos);
    s_msg.retain = retain;

    uint16_t len = 0;
    if (!hap_json_encode_mqtt_publish(s_buf, kBufSz, &len, s_msg)) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "mqtt_publish encode failed topic=%s", topic);
        return;
    }

    HapFrame f{};
    f.type        = HapMsgType::MQTT_PUBLISH;
    f.seq         = hap_session_next_seq();
    f.flags       = 0;
    f.payload     = s_buf;
    f.payload_len = len;
    hap_session_send(f);
    xSemaphoreGive(s_mutex);
    ESP_LOGD(TAG, "MQTT_PUBLISH forwarded topic=%s qos=%d retain=%d", topic, qos, retain);
}

bool mqtt_gw_is_connected()                    { return false; }  // N/A on P4
bool mqtt_gw_is_active()                       { return false; }  // N/A on P4
void mqtt_gw_set_rx_callback(mqtt_rx_cb_t)     {}  // N/A on P4
void mqtt_gw_subscribe(const char*, int)        {}  // N/A on P4
void mqtt_gw_set_broker_url(const char*)        {}  // N/A on P4
void mqtt_gw_set_client_id(const char*)         {}  // N/A on P4
void mqtt_gw_set_root_topic(const char*)        {}  // N/A on P4
const char* mqtt_gw_get_root_topic(void)        { return "zhac"; }
int  mqtt_gw_format_topic(char* out, size_t cap, const char* suffix) {
    // P4 forwards MQTT publishes over HAP; topic is composed S3-side.
    // This stub lets first-party code call format_topic uniformly —
    // it just copies the suffix through so the caller's fall-through
    // fmt still works. Returns -1 on overflow.
    if (!out || cap == 0) return -1;
    const char* sfx = suffix ? suffix : "";
    size_t n = strlen(sfx);
    if (n >= cap) return -1;
    memcpy(out, sfx, n);
    out[n] = '\0';
    return (int)n;
}
