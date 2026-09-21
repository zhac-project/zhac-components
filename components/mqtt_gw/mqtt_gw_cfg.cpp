// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// mqtt_gw_cfg.cpp — see mqtt_gw_cfg.h. Moved here from zhac-wired-core's
// mqtt_glue.cpp so the single-chip build shares it.
#include "mqtt_gw_cfg.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "mqtt_gw.h"
#include "nvs.h"

static const char* TAG = "mqtt_cfg";

namespace {

constexpr const char* kNs = "mqtt_cfg";   // same namespace + keys as net-core

struct Cfg {
    uint8_t enabled = 0;
    char    url[128] = {};
    char    root[32] = {};
    char    cid[32]  = {};
};

// A broker URL, root topic or client id is printable ASCII or it is nothing:
// ArduinoJson writes other bytes into status JSON unescaped, and the web page
// cannot parse that. Anything else is dropped as a whole.
void clean_ascii(char* s) {
    for (char* p = s; *p; p++) {
        if (static_cast<unsigned char>(*p) < 0x20 || static_cast<unsigned char>(*p) > 0x7E) { s[0] = '\0'; return; }
    }
}

Cfg read_cfg() {
    Cfg c;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return c;
    nvs_get_u8(h, "enabled", &c.enabled);
    size_t n = sizeof(c.url);  nvs_get_str(h, "broker_url", c.url, &n);
    n = sizeof(c.root);        nvs_get_str(h, "root_topic", c.root, &n);
    n = sizeof(c.cid);         nvs_get_str(h, "client_id", c.cid, &n);
    nvs_close(h);
    clean_ascii(c.url); clean_ascii(c.root); clean_ascii(c.cid);
    return c;
}

void write_str(const char* key, const char* v) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void write_u8(const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

// mqtt://user:pass@host:1883 -> mqtt://host:1883, for status and logs.
void strip_userinfo(const char* in, char* out, size_t cap) {
    out[0] = '\0';
    if (!in || !in[0]) return;
    const char* sep  = strstr(in, "://");
    const char* host = sep ? sep + 3 : in;
    const char* at   = nullptr;
    for (const char* p = host; *p && *p != '/'; ++p) if (*p == '@') at = p;
    snprintf(out, cap, "%.*s%s", (int)(host - in), in, at ? at + 1 : host);
}

}  // namespace

void mqtt_gw_cfg_boot(void) {
    Cfg c = read_cfg();
    if (c.root[0]) mqtt_gw_set_root_topic(c.root);
    if (c.enabled && c.url[0]) mqtt_gw_configure(c.url, c.root, c.cid);
    else if (c.cid[0])         mqtt_gw_set_client_id(c.cid);
    char safe[128];
    strip_userinfo(c.url, safe, sizeof(safe));
    ESP_LOGI(TAG, "MQTT %s broker=%s root=%s", (c.enabled && c.url[0]) ? "enabled" : "disabled",
             safe[0] ? safe : "(none)", mqtt_gw_get_root_topic());
}

void mqtt_gw_cfg_apply(JsonDocument& doc) {
    const char* url  = doc["broker_url"]      | (const char*)nullptr;
    const char* root = doc["mqtt_root_topic"] | (const char*)nullptr;
    const char* cid  = doc["mqtt_client_id"]  | (const char*)nullptr;
    if (root && root[0] && strlen(root) < 32) { write_str("root_topic", root); mqtt_gw_set_root_topic(root); }
    if (cid && cid[0] && strlen(cid) < 32)    { write_str("client_id", cid);   mqtt_gw_set_client_id(cid); }
    if (url && url[0] && strlen(url) < 127) {
        write_str("broker_url", url);
        // Only a running client picks it up now; a disabled one gets it from
        // NVS when enabled (mqtt_gw_configure would arm it).
        if (read_cfg().enabled) mqtt_gw_set_broker_url(url);
    }
    if (doc["mqtt_enabled"].is<bool>()) {
        const bool en = doc["mqtt_enabled"].as<bool>();
        write_u8("enabled", en ? 1 : 0);
        if (en) {
            const Cfg c = read_cfg();
            if (c.url[0]) { mqtt_gw_configure(c.url, c.root, c.cid); mqtt_gw_on_sta_up(); }
        } else {
            mqtt_gw_stop();
        }
    }
}

void mqtt_gw_cfg_fill_status(JsonObject d) {
    // Not const: ArduinoJson keeps a `const char*` by reference but copies a
    // `char*`, and `c` dies before the document is serialised.
    Cfg c = read_cfg();
    char safe[128];
    strip_userinfo(c.url, safe, sizeof(safe));
    d["mqtt_enabled"]   = c.enabled != 0;
    d["mqtt_broker"]    = safe;
    d["mqtt_client_id"] = c.cid;
}
