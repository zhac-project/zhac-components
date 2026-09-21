// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_bridge.cpp — see ha_bridge.h. The payloads come from ha_discovery.cpp.
#include "ha_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ArduinoJson.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"   // xTaskCreateWithCaps
#include "freertos/task.h"
#include "mqtt_gw.h"
#include "nvs.h"

static const char* TAG = "ha_bridge";

namespace {

constexpr const char* kNvsNs        = "mqtt_cfg";   // beside the broker settings
constexpr size_t      kPrefixCap    = 32;
constexpr size_t      kMaxDevices   = 256;
constexpr int         kQueueDepth   = 24;
constexpr uint32_t    kTaskStack    = 8192;
// A battery device that has said nothing for this long is reported offline
// (zigbee2mqtt's default for passive devices). Mains devices are never timed
// out here: they only report on change, so silence proves nothing.
// ponytail: a ZCL read "ping" would let mains devices go offline too.
constexpr uint32_t    kPassiveSilenceS = 25 * 3600;

enum class Op : uint8_t { RepublishAll, Device, Removed, Command, RetractAll };
struct Msg {
    Op       op;
    uint64_t ieee;
    char     key[32];
    char     value[64];
};

// Config topics published per device, so they can be retracted after the
// device is gone (the firmware no longer knows its exposes by then). One
// '\n'-separated heap string per device, in PSRAM when there is some.
struct Published {
    uint64_t ieee;
    char*    topics;
    bool     passive;         // has the per-device availability topic
    bool     offline;         // what we last told the broker
    volatile uint32_t last_seen_s;   // written from the state publisher's task; 32-bit, atomic
};

const HaBridgePlatform* s_plat = nullptr;
QueueHandle_t           s_q    = nullptr;
volatile bool           s_enabled = false;
char                    s_prefix[kPrefixCap] = "homeassistant";
Published*              s_pub  = nullptr;
char*                   s_bridge_topic = nullptr;   // hub entity, retracted with the rest

void* ext_alloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

char* dup_str(const char* s) {
    const size_t n = strlen(s) + 1;
    char* p = static_cast<char*>(ext_alloc(n));
    if (p) memcpy(p, s, n);
    return p;
}

// The hub's id on the broker: the root topic with anything HA does not accept
// in an id replaced — "home/zhac-garage" -> "home_zhac-garage".
void bridge_id(char* out, size_t cap) {
    const char* r = mqtt_gw_get_root_topic();
    size_t i = 0;
    for (; r[i] && i + 1 < cap; i++) {
        const char ch = r[i];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        out[i] = ok ? ch : '_';
    }
    out[i] = '\0';
}

// Publish with back-pressure: the mqtt_gw queue is 16 deep and a full
// discovery pass is dozens of messages. The leading '/' tells mqtt_gw the
// topic is absolute (no root prefix) — discovery lives under <prefix>/.
bool publish(const char* topic, const char* payload, int qos, bool retain) {
    char abs[168];
    const char* t = topic;
    if (strncmp(topic, mqtt_gw_get_root_topic(), strlen(mqtt_gw_get_root_topic())) != 0) {
        snprintf(abs, sizeof(abs), "/%s", topic);
        t = abs;
    }
    for (int i = 0; i < 40; i++) {
        if (!mqtt_gw_is_connected()) return false;
        if (mqtt_gw_publish(t, payload, strlen(payload), qos, retain)) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "gave up publishing %s", topic);
    return false;
}

Published* slot_for(uint64_t ieee, bool create) {
    if (!s_pub) return nullptr;
    Published* empty = nullptr;
    for (size_t i = 0; i < kMaxDevices; i++) {
        if (s_pub[i].ieee == ieee) return &s_pub[i];
        if (!empty && s_pub[i].ieee == 0) empty = &s_pub[i];
    }
    if (create && empty) empty->ieee = ieee;
    return create ? empty : nullptr;
}

void retract_topics(const char* topics) {
    if (!topics) return;
    const char* p = topics;
    while (*p) {
        const char* nl = strchr(p, '\n');
        const size_t n = nl ? static_cast<size_t>(nl - p) : strlen(p);
        char t[160];
        if (n < sizeof(t)) {
            memcpy(t, p, n);
            t[n] = '\0';
            publish(t, "", 1, true);   // empty retained config = entity removed
        }
        if (!nl) break;
        p = nl + 1;
    }
}

void retract_device(uint64_t ieee) {
    Published* s = slot_for(ieee, false);
    if (!s) return;
    retract_topics(s->topics);
    if (s->passive) {
        // Clear the retained availability too, or HA keeps a stale value
        // should the device ever be re-added under this address.
        char topic[96];
        if (ha::device_availability_topic(topic, sizeof(topic), mqtt_gw_get_root_topic(), s->ieee) >= 0)
            publish(topic, "", 1, true);
    }
    free(s->topics);
    *s = Published{};
}

// Collects a device's config topics while ha::build_device emits them.
struct Collect {
    char   buf[2048];
    size_t len = 0;
};

void emit_config(const char*, const char* topic, const char* payload, void* user) {
    auto* c = static_cast<Collect*>(user);
    if (!publish(topic, payload, 1, true)) return;
    const size_t n = strlen(topic);
    if (c->len + n + 2 < sizeof(c->buf)) {
        memcpy(c->buf + c->len, topic, n);
        c->len += n;
        c->buf[c->len++] = '\n';
        c->buf[c->len] = '\0';
    }
}

void publish_attrs(uint64_t ieee, const char* attrs_json);

uint32_t now_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

void publish_availability(uint64_t ieee, bool online) {
    char topic[96];
    if (ha::device_availability_topic(topic, sizeof(topic), mqtt_gw_get_root_topic(), ieee) < 0) return;
    publish(topic, online ? "online" : "offline", 1, true);
}

void publish_device(const HaDeviceSnapshot& d, void*) {
    if (!s_enabled || !d.ieee) return;
    char bid[kPrefixCap];
    bridge_id(bid, sizeof(bid));
    const ha::Context ctx{s_prefix, mqtt_gw_get_root_topic(), bid};
    auto* col = static_cast<Collect*>(ext_alloc(sizeof(Collect)));
    if (!col) return;
    new (col) Collect();
    bool passive = false;
    const int n = ha::build_device({d.ieee, d.name, d.vendor, d.model, d.exposes, d.battery_powered},
                                   ctx, emit_config, col, &passive);
    if (n < 0) ESP_LOGW(TAG, "0x%016llx: exposes are not a JSON array", (unsigned long long)d.ieee);
    if (Published* s = slot_for(d.ieee, true)) {
        free(s->topics);
        s->topics  = col->len ? dup_str(col->buf) : nullptr;
        s->passive = passive;
        if (passive) {
            // Assume alive on (re)publish; the silence clock starts now. A
            // device already timed out stays offline until it reports.
            if (!s->last_seen_s) s->last_seen_s = now_s();
            s->offline = (now_s() - s->last_seen_s) > kPassiveSilenceS;
            publish_availability(d.ieee, !s->offline);
        }
    }
    free(col);
    publish_attrs(d.ieee, d.attrs);   // HA shows the current values right away
}

// Battery devices silent for too long go offline; the first report brings
// them back. Runs on the bridge task once a second.
void tick_availability() {
    if (!s_enabled || !s_pub || !mqtt_gw_is_connected()) return;
    const uint32_t now = now_s();
    for (size_t i = 0; i < kMaxDevices; i++) {
        Published& p = s_pub[i];
        if (!p.ieee || !p.passive) continue;
        const bool silent = (now - p.last_seen_s) > kPassiveSilenceS;
        if (silent != p.offline) {
            p.offline = silent;
            publish_availability(p.ieee, !silent);
            ESP_LOGI(TAG, "0x%016llx %s", (unsigned long long)p.ieee, silent ? "silent for a day: offline" : "back: online");
        }
    }
}

void publish_bridge() {
    char bid[kPrefixCap];
    bridge_id(bid, sizeof(bid));
    const ha::Context ctx{s_prefix, mqtt_gw_get_root_topic(), bid};
    const esp_app_desc_t* app = esp_app_get_description();
    ha::build_bridge(ctx, app ? app->version : "", s_plat ? s_plat->hub_model : "",
        [](const char*, const char* topic, const char* payload, void*) {
            if (!publish(topic, payload, 1, true)) return;
            free(s_bridge_topic);
            s_bridge_topic = dup_str(topic);
        }, nullptr);
}

void republish_all() {
    if (!s_enabled || !s_plat || !mqtt_gw_is_connected()) return;
    ESP_LOGI(TAG, "publishing discovery under %s/", s_prefix);
    publish_bridge();
    s_plat->for_each_device(publish_device, nullptr);
}

void retract_all() {
    retract_topics(s_bridge_topic);
    free(s_bridge_topic);
    s_bridge_topic = nullptr;
    for (size_t i = 0; s_pub && i < kMaxDevices; i++) {
        if (s_pub[i].ieee) retract_device(s_pub[i].ieee);
    }
}

void handle_command(const Msg& m) {
    if (!s_plat || !s_plat->set_attr) return;
    if (!s_plat->set_attr(m.ieee, m.key, m.value)) {
        ESP_LOGW(TAG, "0x%016llx %s <- %s: rejected", (unsigned long long)m.ieee, m.key, m.value);
    }
}

void task(void*) {
    bool was_connected = false;
    for (;;) {
        Msg m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (m.op) {
                case Op::RepublishAll: republish_all(); break;
                case Op::RetractAll:   retract_all(); break;
                case Op::Removed:      retract_device(m.ieee); break;
                case Op::Command:      handle_command(m); break;
                case Op::Device:
                    if (s_enabled && s_plat && mqtt_gw_is_connected())
                        s_plat->get_device(m.ieee, publish_device, nullptr);
                    break;
            }
        }
        // mqtt_gw has no connect callback: watch for the edge. A reconnect
        // means a new session, maybe a restarted broker — publish everything.
        // ponytail: 1 s poll; a CONNECTED hook in mqtt_gw if this ever matters.
        const bool connected = mqtt_gw_is_connected();
        if (connected && !was_connected) republish_all();
        was_connected = connected;
        tick_availability();
    }
}

bool post(Op op, uint64_t ieee = 0, const char* key = nullptr, const char* value = nullptr) {
    if (!s_q) return false;
    Msg m{};
    m.op = op;
    m.ieee = ieee;
    if (key)   snprintf(m.key, sizeof(m.key), "%s", key);
    if (value) snprintf(m.value, sizeof(m.value), "%s", value);
    return xQueueSend(s_q, &m, 0) == pdTRUE;
}

// Values arrive as JSON text; publish them as HA matches them. "action" (a
// button press, an HA event entity) goes without retain: a retained press
// would fire again on every Home Assistant restart.
void publish_value(uint64_t ieee, const char* key, const char* value_json) {
    char topic[128];
    char payload[64];
    if (ha::state_topic(topic, sizeof(topic), mqtt_gw_get_root_topic(), ieee, key) < 0) return;
    if (!ha::state_payload(value_json, payload, sizeof(payload))) return;
    mqtt_gw_publish(topic, payload, strlen(payload), 0, strcmp(key, "action") != 0);
}

void publish_attrs(uint64_t ieee, const char* attrs_json) {
    if (!attrs_json || !attrs_json[0]) return;
    JsonDocument doc;
    if (deserializeJson(doc, attrs_json) || !doc.is<JsonObjectConst>()) return;
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const char* key = kv.key().c_str();
        if (!key || key[0] == '_') continue;          // internal keys
        char val[64];
        const size_t n = serializeJson(kv.value(), val, sizeof(val));
        if (n == 0 || n >= sizeof(val)) continue;
        publish_value(ieee, key, val);
    }
}

}  // namespace

void ha_bridge_init(const HaBridgePlatform* platform) {
    s_plat = platform;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0;
        nvs_get_u8(h, "ha_disc", &en);
        s_enabled = en != 0;
        size_t n = sizeof(s_prefix);
        if (nvs_get_str(h, "ha_prefix", s_prefix, &n) != ESP_OK || !s_prefix[0])
            snprintf(s_prefix, sizeof(s_prefix), "homeassistant");
        nvs_close(h);
    }
    if (!s_pub) {
        s_pub = static_cast<Published*>(ext_alloc(sizeof(Published) * kMaxDevices));
        if (s_pub) memset(s_pub, 0, sizeof(Published) * kMaxDevices);
    }
    if (!s_q) {
        s_q = xQueueCreate(kQueueDepth, sizeof(Msg));
        // Stack in PSRAM: internal RAM is the tight budget on every ZHAC chip,
        // and this task never writes flash (NVS writes happen in the caller of
        // ha_bridge_configure), which is what a PSRAM stack must not do.
        BaseType_t ok = s_q ? xTaskCreateWithCaps(task, "ha_bridge", kTaskStack, nullptr,
                                                  tskIDLE_PRIORITY + 2, nullptr,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                            : pdFAIL;
        if (s_q && ok != pdPASS)
            ok = xTaskCreate(task, "ha_bridge", kTaskStack, nullptr, tskIDLE_PRIORITY + 2, nullptr);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "task start failed -- Home Assistant discovery unavailable");
            return;
        }
    }
    ESP_LOGI(TAG, "Home Assistant discovery %s (prefix %s)", s_enabled ? "on" : "off", s_prefix);
}

bool        ha_bridge_enabled(void) { return s_enabled; }
const char* ha_bridge_prefix(void)  { return s_prefix; }

void ha_bridge_configure(bool enabled, const char* prefix) {
    char next[kPrefixCap];
    snprintf(next, sizeof(next), "%s", (prefix && prefix[0]) ? prefix : s_prefix);
    // The prefix becomes a topic level and is echoed inside JSON status:
    // keep it to what Home Assistant itself accepts, [A-Za-z0-9_-].
    for (char* c = next; *c; c++) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
        if (!ok) *c = '_';
    }
    const bool prefix_changed = strcmp(next, s_prefix) != 0;
    // Retraction replays the full topics recorded at publish time, so the
    // prefix can move right away; queue order keeps retract before republish.
    if (s_enabled && (!enabled || prefix_changed)) post(Op::RetractAll);
    snprintf(s_prefix, sizeof(s_prefix), "%s", next);
    s_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ha_disc", enabled ? 1 : 0);
        nvs_set_str(h, "ha_prefix", s_prefix);
        nvs_commit(h);
        nvs_close(h);
    }
    if (enabled) post(Op::RepublishAll);
    ESP_LOGI(TAG, "Home Assistant discovery %s (prefix %s)", enabled ? "on" : "off", s_prefix);
}

void ha_bridge_device_changed(uint64_t ieee) {
    if (s_enabled) post(Op::Device, ieee);
}

void ha_bridge_device_removed(uint64_t ieee) {
    post(Op::Removed, ieee);
}

void ha_bridge_publish_state(uint64_t ieee, const char* key, const char* value_json) {
    if (!s_enabled || !key || !value_json || key[0] == '_' || !mqtt_gw_is_connected()) return;
    // Heard from: the bridge task notices a timed-out device is back.
    if (Published* s = slot_for(ieee, false)) s->last_seen_s = now_s();
    publish_value(ieee, key, value_json);
}

bool ha_bridge_on_mqtt_rx(const char* topic, int topic_len, const char* data, int data_len) {
    if (!s_enabled || !topic || topic_len <= 0) return false;
    uint64_t ieee = 0;
    char key[32];
    if (!ha::parse_command_topic(topic, static_cast<size_t>(topic_len), mqtt_gw_get_root_topic(),
                                 &ieee, key, sizeof(key)))
        return false;
    char value[64];
    if (!ha::command_value_json(data ? data : "", data_len > 0 ? static_cast<size_t>(data_len) : 0,
                                value, sizeof(value)))
        return true;   // ours, but unusable: drop it
    if (!post(Op::Command, ieee, key, value)) ESP_LOGW(TAG, "command queue full, dropped %s", key);
    return true;
}
