// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/hap_json/hap_json.cpp
#include "hap_json.h"
#include "ArduinoJson.h"
#include "esp_log.h"
#include <cstring>
#include <cinttypes>
#include <cstdio>

static const char* TAG = "hap_json";

// Helper: format IEEE as "0x%016" PRIx64 string (20 bytes incl. null)
static void fmt_ieee(char* out, uint64_t ieee) {
    snprintf(out, 20, "0x%016" PRIX64, ieee);
}

// Escape a NUL-terminated string for embedding inside a JSON string
// value. See hap_json.h for the contract. Used by every snprintf-style
// JSON builder that interpolates untrusted bytes (device-reported
// strings, SSIDs, alert messages, etc.) — without escaping a stray `"`
// or control byte poisons the whole response.
size_t hap_json_escape_str(const char* src, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    if (!src) { out[0] = '\0'; return 0; }
    size_t wi = 0;
    // Reserve 1 byte for the trailing NUL throughout.
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (wi + 2 + 1 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = (char)c;
        } else if (c == '\n') {
            if (wi + 2 + 1 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 'n';
        } else if (c == '\r') {
            if (wi + 2 + 1 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 'r';
        } else if (c == '\t') {
            if (wi + 2 + 1 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 't';
        } else if (c < 0x20) {
            // Drop other control bytes (matches ws_bridge legacy
            // behaviour). Encoding as \u00XX would also be valid but
            // wastes 6 bytes per stray byte and these never occur in
            // the strings we currently feed through here.
        } else {
            if (wi + 1 + 1 > out_cap) break;
            out[wi++] = (char)c;
        }
    }
    out[wi] = '\0';
    return wi;
}

// Helper: parse "0x..." or decimal string to uint64_t
static uint64_t parse_ieee(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return strtoull(s + 2, nullptr, 16);
    return strtoull(s, nullptr, 10);
}

// ── SYNC ──────────────────────────────────────────────────────────────────
bool hap_json_encode_sync_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                               uint32_t session_id, const char* fw_ver) {
    JsonDocument doc;
    doc["proto_ver"]  = 1;
    doc["fw_ver"]     = fw_ver;
    doc["session_id"] = session_id;
    doc["is_ack"]     = false;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_sync_req json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_sync_req buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_encode_sync_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                               uint32_t session_id, const char* fw_ver, uint16_t device_count) {
    JsonDocument doc;
    doc["proto_ver"]    = 1;
    doc["fw_ver"]       = fw_ver;
    doc["session_id"]   = session_id;
    doc["device_count"] = device_count;
    doc["is_ack"]       = true;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_sync_ack json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_sync_ack buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_sync(const uint8_t* payload, uint16_t len, HapSyncInfo& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    out.session_id    = doc["session_id"] | 0u;
    out.device_count  = doc["device_count"] | 0u;
    out.is_ack        = doc["is_ack"] | false;
    const char* fv    = doc["fw_ver"] | "";
    strncpy(out.fw_ver, fv, sizeof(out.fw_ver) - 1);
    out.fw_ver[sizeof(out.fw_ver) - 1] = '\0';
    return true;
}

// ── HEARTBEAT ─────────────────────────────────────────────────────────────
bool hap_json_encode_heartbeat(uint8_t* buf, size_t cap, uint16_t* out_len,
                                const HapHeartbeat& hb) {
    JsonDocument doc;
    doc["uptime"]      = hb.uptime;
    doc["heap"]        = hb.heap;
    doc["psram_free"]  = hb.psram_free;
    doc["psram_total"] = hb.psram_total;
    doc["cpu_c0"]      = hb.cpu_pct_c0;
    doc["cpu_c1"]      = hb.cpu_pct_c1;
    doc["proto_mask"]  = hb.proto_mask;
    // Extended memory diagnostics.
    doc["heap_min"]    = hb.heap_min_free;
    doc["int_free"]    = hb.internal_free;
    doc["int_min"]     = hb.internal_min_free;
    doc["int_blk"]     = hb.internal_largest_block;
    doc["psram_min"]   = hb.psram_min_free;
    doc["psram_blk"]   = hb.psram_largest_block;
    doc["stack_hwm"]   = hb.task_stack_hwm_bytes;
    doc["dev_count"]   = hb.device_count;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_heartbeat json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_heartbeat buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_heartbeat(const uint8_t* payload, uint16_t len, HapHeartbeat& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    out.uptime      = doc["uptime"]      | 0u;
    out.cpu_pct_c0  = doc["cpu_c0"]      | 0u;
    out.cpu_pct_c1  = doc["cpu_c1"]      | 0u;
    out.heap        = doc["heap"]        | 0u;
    out.psram_free  = doc["psram_free"]  | 0u;
    out.psram_total = doc["psram_total"] | 0u;
    out.proto_mask  = doc["proto_mask"]  | 0u;
    out.heap_min_free           = doc["heap_min"]  | 0u;
    out.internal_free           = doc["int_free"]  | 0u;
    out.internal_min_free       = doc["int_min"]   | 0u;
    out.internal_largest_block  = doc["int_blk"]   | 0u;
    out.psram_min_free          = doc["psram_min"] | 0u;
    out.psram_largest_block     = doc["psram_blk"] | 0u;
    out.task_stack_hwm_bytes    = doc["stack_hwm"] | 0u;
    out.device_count            = doc["dev_count"] | 0u;
    return true;
}

// ── DEVICE_LIST ───────────────────────────────────────────────────────────
bool hap_json_encode_device_list(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const ZapDevice* devs, uint16_t count,
                                  HapJsonLabelResolverFn resolve) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (uint16_t i = 0; i < count; i++) {
        // Hide soft-removed entries. The pool slot is kept so a rejoin
        // can restore configure state without rediscovery, but the UI
        // should treat them as gone until we see them again.
        if (zap_dev_is_removed(&devs[i])) continue;
        JsonObject o = arr.add<JsonObject>();
        char ieee_s[20]; fmt_ieee(ieee_s, devs[i].ieee_addr);
        o["ieee"]         = ieee_s;
        o["proto"]        = ncp_protocol_name(devs[i].protocol);
        o["nwk"]          = devs[i].nwk_addr;
        o["name"]         = devs[i].friendly_name;
        o["type"]         = devs[i].device_type;
        o["mfr"]          = devs[i].manufacturer_code;       // numeric Zigbee manu id
        o["manufacturer"] = devs[i].manufacturer_name;       // Basic attr 0x0004 raw (e.g. `_TZ3000_xwh1e22x`)
        // Friendly vendor / model come from the matched ZHC def if
        // any; fall back to raw device-reported strings when either
        // the resolver is absent or the def misses. Buffers sized to
        // cover every vendor/model string we ship today (biggest is
        // `_TZ3000_xwh1e22x` at 16 + terminator).
        char vendor_buf[32] = {};
        char model_buf[32]  = {};
        if (resolve) resolve(&devs[i], vendor_buf, sizeof(vendor_buf),
                                          model_buf,  sizeof(model_buf));
        o["vendor"] = vendor_buf[0]
                       ? (const char*)vendor_buf
                       : (const char*)devs[i].manufacturer_name;
        o["model"]  = model_buf[0]
                       ? (const char*)model_buf
                       : (const char*)devs[i].model_id;
        o["lqi"]          = devs[i].link_quality;
        o["last_seen"]    = devs[i].last_seen;
        o["ps"]           = devs[i].power_source;
        o["bat_pct"]      = devs[i].battery_pct;
        o["ep_count"]     = devs[i].endpoint_count;
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_device_list json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_device_list buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

// ── ALERT (0x23) ──────────────────────────────────────────────────────────
bool hap_json_encode_alert(uint8_t* buf, size_t cap, uint16_t* out_len,
                            const HapAlert& a) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, a.ieee);
    doc["code"] = static_cast<uint8_t>(a.code);
    doc["ieee"] = ieee_s;
    doc["msg"]  = a.msg;
    doc["ts"]   = a.ts;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_alert overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_alert(const uint8_t* payload, uint16_t len, HapAlert& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    out.code = static_cast<HapAlertCode>(doc["code"] | 0u);
    const char* s = doc["ieee"] | "0x0";
    out.ieee = strtoull(s, nullptr, 16);
    const char* m = doc["msg"] | "";
    strncpy(out.msg, m, sizeof(out.msg) - 1);
    out.msg[sizeof(out.msg) - 1] = '\0';
    out.ts = doc["ts"] | 0u;
    return true;
}

// ── GET_DEVICE_BY_ID / DEVICE_INFO ────────────────────────────────────────
bool hap_json_encode_get_device_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     uint64_t ieee) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ieee);
    doc["ieee"] = ieee_s;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_get_device_req overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_get_device_req(const uint8_t* payload, uint16_t len,
                                     uint64_t* ieee_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* s = doc["ieee"] | "";
    if (s[0] == '\0') return false;
    *ieee_out = strtoull(s, nullptr, 16);
    return true;
}

bool hap_json_encode_device_info(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const ZapDevice* dev,
                                  HapJsonLabelResolverFn resolve) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, dev->ieee_addr);
    doc["ok"]           = true;
    doc["ieee"]         = ieee_s;
    doc["proto"]        = ncp_protocol_name(dev->protocol);
    doc["nwk"]          = dev->nwk_addr;
    doc["name"]         = dev->friendly_name;
    doc["type"]         = dev->device_type;
    doc["last_seen"]    = dev->last_seen;
    doc["mfr"]          = dev->manufacturer_code;      // numeric Zigbee manu id
    doc["manufacturer"] = dev->manufacturer_name;      // Basic attr 0x0004 raw
    // Friendly vendor / model — see device_list above.
    char vendor_buf[32] = {};
    char model_buf[32]  = {};
    if (resolve) resolve(dev, vendor_buf, sizeof(vendor_buf),
                               model_buf,  sizeof(model_buf));
    doc["vendor"] = vendor_buf[0]
                     ? (const char*)vendor_buf
                     : (const char*)dev->manufacturer_name;
    doc["model"]  = model_buf[0]
                     ? (const char*)model_buf
                     : (const char*)dev->model_id;
    doc["lqi"]          = dev->link_quality;
    doc["bat_pct"]      = dev->battery_pct;
    doc["ep_count"]     = dev->endpoint_count;
    JsonArray eps = doc["eps"].to<JsonArray>();
    for (uint8_t i = 0; i < dev->endpoint_count && i < 8; i++) {
        eps.add(dev->endpoints[i]);
    }
    JsonArray cls = doc["clusters"].to<JsonArray>();
    for (uint8_t i = 0; i < dev->endpoint_count && i < 8; i++) {
        JsonArray row = cls.add<JsonArray>();
        for (uint8_t j = 0; j < ZAP_CLUSTERS_PER_EP; j++) {
            if (dev->clusters[i][j] != 0) row.add(dev->clusters[i][j]);
        }
    }
    JsonArray cls_out = doc["clusters_out"].to<JsonArray>();
    for (uint8_t i = 0; i < dev->endpoint_count && i < 8; i++) {
        JsonArray row = cls_out.add<JsonArray>();
        for (uint8_t j = 0; j < ZAP_CLUSTERS_PER_EP; j++) {
            if (dev->clusters_out[i][j] != 0) row.add(dev->clusters_out[i][j]);
        }
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_device_info overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_encode_device_info_full(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const ZapDevice* dev,
                                       HapJsonLabelResolverFn resolve,
                                       HapJsonAttrsEmitter   attrs,
                                       HapJsonExposesEmitter exposes) {
    if (!hap_json_encode_device_info(buf, cap, out_len, dev, resolve)) return false;
    if (!dev) return true;

    uint16_t len = *out_len;
    auto splice = [&](const char* key, size_t (*emit)(const ZapDevice*, char*, size_t)) -> bool {
        if (!emit) return true;  // skipped intentionally — not a failure
        if (len == 0 || buf[len - 1] != '}') return false;
        const uint16_t saved = len;
        len--;  // strip outer '}'
        int w = snprintf(reinterpret_cast<char*>(buf) + len, cap - len,
                          ",\"%s\":", key);
        if (w <= 0 || (size_t)w >= cap - len) { buf[saved - 1] = '}'; len = saved; return false; }
        len += static_cast<uint16_t>(w);
        // Reserve one byte for the trailing '}' we re-append below.
        const size_t emit_cap = (cap > (size_t)len + 1) ? cap - len - 1 : 0;
        size_t written = emit(dev, reinterpret_cast<char*>(buf) + len, emit_cap);
        if (written == 0 || written > emit_cap) { buf[saved - 1] = '}'; len = saved; return false; }
        len += static_cast<uint16_t>(written);
        buf[len++] = '}';
        if (len < cap) buf[len] = '\0';
        return true;
    };
    if (!splice("attrs", attrs)) {
        ESP_LOGW(TAG, "device_info splice('attrs') reverted — base len=%u cap=%u",
                  *out_len, (unsigned)cap);
    }
    if (!splice("exposes", exposes)) {
        ESP_LOGW(TAG, "device_info splice('exposes') reverted — len=%u cap=%u",
                  len, (unsigned)cap);
    }

    *out_len = len;
    return true;
}

bool hap_json_encode_device_info_err(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* err) {
    JsonDocument doc;
    doc["ok"]  = false;
    doc["err"] = err;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) return false;
    *out_len = static_cast<uint16_t>(n);
    return true;
}

// ── DEVICE_SET_NAME ───────────────────────────────────────────────────────
bool hap_json_encode_device_set_name(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      uint64_t ieee, const char* name) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ieee);
    doc["ieee"] = ieee_s;
    doc["name"] = name;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_device_set_name overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_device_set_name(const uint8_t* payload, uint16_t len,
                                      uint64_t* ieee_out, char* name_out, size_t name_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* s = doc["ieee"] | "";
    if (s[0] == '\0') return false;
    const char* n = doc["name"] | "";
    *ieee_out = strtoull(s, nullptr, 16);
    strncpy(name_out, n, name_max - 1);
    name_out[name_max - 1] = '\0';
    return true;
}

// ── DEVICE_JOIN / DEVICE_LEAVE ────────────────────────────────────────────
bool hap_json_encode_device_join(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint64_t ieee) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ieee);
    doc["ieee"] = ieee_s;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_device_join overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_device_join(const uint8_t* payload, uint16_t len,
                                  uint64_t* ieee_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* s = doc["ieee"] | "";
    if (s[0] == '\0') return false;
    *ieee_out = strtoull(s, nullptr, 16);
    return true;
}

// ── SET_ATTRIBUTE ─────────────────────────────────────────────────────────
bool hap_json_encode_set_attr(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const HapSetAttrReq& req) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, req.ieee);
    doc["ieee"] = ieee_s;
    doc["ep"]   = req.ep;
    doc["cl"]   = req.cluster;
    doc["at"]   = req.attr;
    doc["val"]  = req.val;
    if (req.key[0]) doc["key"] = req.key;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_set_attr overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_set_attr(const uint8_t* payload, uint16_t len, HapSetAttrReq& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* ieee_s = doc["ieee"] | "0x0";
    out.ieee    = parse_ieee(ieee_s);
    out.ep      = doc["ep"]  | 0u;
    out.cluster = doc["cl"]  | 0u;
    out.attr    = doc["at"]  | 0u;
    out.val     = doc["val"] | 0;
    const char* k = doc["key"] | "";
    strncpy(out.key, k, sizeof(out.key) - 1);
    out.key[sizeof(out.key) - 1] = '\0';
    return true;
}

bool hap_json_encode_set_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                              uint64_t ieee, bool ok) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ieee);
    doc["ieee"] = ieee_s;
    doc["ok"]   = ok;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_set_ack json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_set_ack buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

// ── DEVICE_EVENT ──────────────────────────────────────────────────────────
bool hap_json_encode_device_event(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const HapDeviceEvent& ev) {
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ev.ieee);
    doc["ieee"] = ieee_s;
    doc["cl"]   = ev.cluster;
    doc["at"]   = ev.attr;
    doc["val"]  = ev.val;
    doc["vt"]   = ev.val_type;
    doc["ts"]   = ev.ts;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_device_event json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_device_event buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

// ── RULE management ───────────────────────────────────────────────────────

bool hap_json_encode_rule_create(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const char* name, const char* dsl) {
    JsonDocument doc;
    doc["name"] = name ? name : "";
    doc["dsl"]  = dsl;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_create overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_create(const uint8_t* payload, uint16_t len,
                                  char* name_out, size_t name_max,
                                  char* dsl_out,  size_t dsl_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* dsl = doc["dsl"] | "";
    if (dsl[0] == '\0') return false;
    strncpy(dsl_out, dsl, dsl_max - 1);
    dsl_out[dsl_max - 1] = '\0';
    if (name_out && name_max > 0) {
        const char* nm = doc["name"] | "";
        strncpy(name_out, nm, name_max - 1);
        name_out[name_max - 1] = '\0';
    }
    return true;
}

bool hap_json_encode_rule_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint16_t rule_id) {
    JsonDocument doc;
    doc["id"] = rule_id;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_delete overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_delete(const uint8_t* payload, uint16_t len,
                                  uint16_t* rule_id_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    if (!doc["id"].is<uint16_t>()) return false;
    *rule_id_out = doc["id"].as<uint16_t>();
    return true;
}

bool hap_json_encode_rule_update(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint16_t rule_id, bool enabled) {
    JsonDocument doc;
    doc["id"]      = rule_id;
    doc["enabled"] = enabled;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_update overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_update(const uint8_t* payload, uint16_t len,
                                  uint16_t* rule_id_out, bool* enabled_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    if (!doc["id"].is<uint16_t>()) return false;
    *rule_id_out  = doc["id"].as<uint16_t>();
    *enabled_out  = doc["enabled"] | false;
    return true;
}

bool hap_json_encode_rule_update_dsl(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      uint16_t rule_id, const char* name,
                                      const char* dsl) {
    JsonDocument doc;
    doc["id"]   = rule_id;
    doc["name"] = name ? name : "";
    doc["dsl"]  = dsl;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_update_dsl overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_update_dsl(const uint8_t* payload, uint16_t len,
                                      uint16_t* rule_id_out,
                                      char* name_out, size_t name_max,
                                      char* dsl_out,  size_t dsl_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    if (!doc["id"].is<uint16_t>()) return false;
    const char* dsl = doc["dsl"] | "";
    if (dsl[0] == '\0') return false;
    *rule_id_out = doc["id"].as<uint16_t>();
    strncpy(dsl_out, dsl, dsl_max - 1);
    dsl_out[dsl_max - 1] = '\0';
    if (name_out && name_max > 0) {
        const char* nm = doc["name"] | "";
        strncpy(name_out, nm, name_max - 1);
        name_out[name_max - 1] = '\0';
    }
    return true;
}

bool hap_json_encode_rule_list_req(uint8_t* buf, size_t cap, uint16_t* out_len) {
    if (cap < 3) return false;
    buf[0] = '{'; buf[1] = '}'; buf[2] = '\0';
    *out_len = 2;
    return true;
}

bool hap_json_encode_rule_list_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    const HapRuleSlotInfo* slots, uint16_t count) {
    JsonDocument doc;
    JsonArray arr = doc["rules"].to<JsonArray>();
    for (uint16_t i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]      = slots[i].rule_id;
        o["type"]    = slots[i].rule_type;
        o["enabled"] = slots[i].enabled;
        o["name"]    = slots[i].name;
        o["src"]     = slots[i].src;
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_list_rsp overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_list_rsp(const uint8_t* payload, uint16_t len,
                                    HapRuleSlotInfo* slots, uint16_t max_slots,
                                    uint16_t* count_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    JsonArray arr = doc["rules"].as<JsonArray>();
    uint16_t n = 0;
    for (JsonObject o : arr) {
        if (n >= max_slots) break;
        slots[n].rule_id   = o["id"]   | 0u;
        slots[n].rule_type = o["type"] | 0u;
        slots[n].enabled   = o["enabled"] | false;
        const char* nm     = o["name"] | "";
        strncpy(slots[n].name, nm, sizeof(slots[n].name) - 1);
        slots[n].name[sizeof(slots[n].name) - 1] = '\0';
        const char* src    = o["src"] | "";
        strncpy(slots[n].src, src, sizeof(slots[n].src) - 1);
        slots[n].src[sizeof(slots[n].src) - 1] = '\0';
        n++;
    }
    *count_out = n;
    return true;
}

bool hap_json_encode_rule_exec_result(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const HapRuleExecResult& r) {
    JsonDocument doc;
    doc["ok"]  = r.ok;
    doc["id"]  = r.rule_id;
    doc["err"] = r.err;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_rule_exec_result overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_rule_exec_result(const uint8_t* payload, uint16_t len,
                                       HapRuleExecResult& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    out.ok      = doc["ok"]  | false;
    out.rule_id = doc["id"]  | 0u;
    const char* err = doc["err"] | "";
    strncpy(out.err, err, sizeof(out.err) - 1);
    out.err[sizeof(out.err) - 1] = '\0';
    return true;
}

// ── SCRIPT management ─────────────────────────────────────────────────────

bool hap_json_encode_script_write(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const char* name, const char* src) {
    if (!name || !*name) return false;
    JsonDocument doc;
    doc["name"] = name;
    doc["src"]  = src;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_write overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_write(const uint8_t* payload, uint16_t len,
                                   char* name_out, size_t name_max,
                                   char* src_out, size_t src_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    if (!*name) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    const char* src = doc["src"] | "";
    strncpy(src_out, src, src_max - 1);
    src_out[src_max - 1] = '\0';
    return true;
}

bool hap_json_encode_script_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    const char* name) {
    if (!name || !*name) return false;
    JsonDocument doc;
    doc["name"] = name;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_delete overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_delete(const uint8_t* payload, uint16_t len,
                                    char* name_out, size_t name_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    if (!*name) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    return true;
}

bool hap_json_encode_script_run_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     const char* name) {
    if (!name || !*name) return false;
    JsonDocument doc;
    doc["name"] = name;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_run_req overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_run_req(const uint8_t* payload, uint16_t len,
                                     char* name_out, size_t name_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    if (!*name) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    return true;
}

bool hap_json_encode_script_check_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const char* name, const char* src) {
    if (!name || !*name || !src) return false;
    JsonDocument doc;
    doc["name"] = name;
    doc["src"]  = src;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_check_req overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_check_req(const uint8_t* payload, uint16_t len,
                                       char* name_out, size_t name_max,
                                       char* src_out,  size_t src_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    const char* src  = doc["src"]  | "";
    if (!*name || !*src) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    strncpy(src_out, src, src_max - 1);
    src_out[src_max - 1] = '\0';
    return true;
}

bool hap_json_encode_script_check_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       bool ok, const char* err, int line) {
    JsonDocument doc;
    doc["ok"] = ok;
    if (err && err[0]) doc["err"] = err;
    if (line > 0)      doc["line"] = line;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_check_rsp overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_check_rsp(const uint8_t* payload, uint16_t len,
                                       bool* ok_out,
                                       char* err_out, size_t err_max,
                                       int*  line_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    if (ok_out)   *ok_out   = doc["ok"]   | false;
    if (line_out) *line_out = doc["line"] | 0;
    if (err_out && err_max) {
        const char* e = doc["err"] | "";
        strncpy(err_out, e, err_max - 1);
        err_out[err_max - 1] = '\0';
    }
    return true;
}

bool hap_json_encode_script_list_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const HapScriptInfo* scripts, uint16_t count) {
    JsonDocument doc;
    JsonArray arr = doc["scripts"].to<JsonArray>();
    for (uint16_t i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = scripts[i].name;
        o["size"] = scripts[i].size;
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_list_rsp overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_list_rsp(const uint8_t* payload, uint16_t len,
                                      HapScriptInfo* scripts, uint16_t max_scripts,
                                      uint16_t* count_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    JsonArray arr = doc["scripts"].as<JsonArray>();
    uint16_t n = 0;
    for (JsonObject o : arr) {
        if (n >= max_scripts) break;
        const char* name = o["name"] | "";
        strncpy(scripts[n].name, name, HAP_SCRIPT_NAME_MAX);
        scripts[n].name[HAP_SCRIPT_NAME_MAX] = '\0';
        scripts[n].size = o["size"] | 0u;
        n++;
    }
    *count_out = n;
    return true;
}

bool hap_json_encode_script_read_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* name) {
    if (!name || !*name) return false;
    JsonDocument doc;
    doc["name"] = name;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_read_req overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_read_req(const uint8_t* payload, uint16_t len,
                                      char* name_out, size_t name_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    if (!*name) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    return true;
}

bool hap_json_encode_script_read_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* name, const char* src) {
    if (!name || !*name) return false;
    JsonDocument doc;
    doc["name"] = name;
    doc["src"]  = src;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_script_read_rsp overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_script_read_rsp(const uint8_t* payload, uint16_t len,
                                      char* name_out, size_t name_max,
                                      char* src_out, size_t src_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* name = doc["name"] | "";
    if (!*name) return false;
    strncpy(name_out, name, name_max - 1);
    name_out[name_max - 1] = '\0';
    const char* src = doc["src"] | "";
    strncpy(src_out, src, src_max - 1);
    src_out[src_max - 1] = '\0';
    return true;
}

// ── PERMIT_JOIN ───────────────────────────────────────────────────────────
bool hap_json_encode_permit_join(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint8_t duration_s) {
    JsonDocument doc;
    doc["duration"] = duration_s;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_permit_join(const uint8_t* payload, uint16_t len,
                                  uint8_t* duration_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    if (!doc["duration"].is<int>()) return false;
    *duration_out = (uint8_t)doc["duration"].as<int>();
    return true;
}

// ── BIND_REQ / BIND_ACK ───────────────────────────────────────────────────
bool hap_json_encode_bind_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const HapBindReq& r) {
    JsonDocument doc;
    char s[20], d[20];
    fmt_ieee(s, r.src_ieee); fmt_ieee(d, r.dst_ieee);
    doc["src_ieee"] = s;
    doc["src_ep"]   = r.src_ep;
    doc["cluster"]  = r.cluster;
    doc["dst_ieee"] = d;
    doc["dst_ep"]   = r.dst_ep;
    doc["unbind"]   = r.unbind;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_bind_req(const uint8_t* payload, uint16_t len,
                               HapBindReq& out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    out.src_ieee = parse_ieee(doc["src_ieee"] | "");
    out.src_ep   = (uint8_t)(doc["src_ep"] | 0);
    out.cluster  = (uint16_t)(doc["cluster"] | 0);
    out.dst_ieee = parse_ieee(doc["dst_ieee"] | "");
    out.dst_ep   = (uint8_t)(doc["dst_ep"] | 0);
    out.unbind   = doc["unbind"] | false;
    return out.src_ieee != 0 && out.dst_ieee != 0;
}

bool hap_json_encode_bind_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                               bool ok) {
    JsonDocument doc;
    doc["ok"] = ok;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

// ── DEVICE_DELETE / DEVICE_DELETE_ACK ────────────────────────────────────
bool hap_json_encode_device_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    uint64_t ieee, bool hard) {
    JsonDocument doc;
    char s[20]; fmt_ieee(s, ieee);
    doc["ieee"] = s;
    if (hard) doc["hard"] = true;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_device_delete(const uint8_t* payload, uint16_t len,
                                    uint64_t* ieee_out, bool* hard_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    *ieee_out = parse_ieee(doc["ieee"] | "");
    if (hard_out) *hard_out = doc["hard"] | false;
    return *ieee_out != 0;
}

bool hap_json_encode_device_delete_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                        bool ok) {
    JsonDocument doc;
    doc["ok"] = ok;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

// ── INTERVIEW_REQ ────────────────────────────────────────────────────────

bool hap_json_encode_interview_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    uint64_t ieee) {
    JsonDocument doc;
    char s[20]; fmt_ieee(s, ieee);
    doc["ieee"] = s;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_interview_req(const uint8_t* payload, uint16_t len,
                                    uint64_t* ieee_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    *ieee_out = parse_ieee(doc["ieee"] | "");
    return *ieee_out != 0;
}

// ── DEVICE_OPTIONS_SET / DEVICE_OPTIONS_SET_ACK ──────────────────────────
// Propagates per-device DeviceConfig knobs that live on P4's device_shadow
// from the S3-side /api/devices/<ieee>/options endpoint. Carries
// `occupancy_timeout`, `debounce_ms`, and `throttle_ms` (all optional).

bool hap_json_encode_device_options_set(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         uint64_t ieee,
                                         const int32_t* occupancy_timeout_s,
                                         const int32_t* debounce_ms,
                                         const int32_t* throttle_ms) {
    JsonDocument doc;
    char s[20]; fmt_ieee(s, ieee);
    doc["ieee"] = s;
    if (occupancy_timeout_s) doc["occupancy_timeout"] = *occupancy_timeout_s;
    if (debounce_ms)         doc["debounce_ms"]       = *debounce_ms;
    if (throttle_ms)         doc["throttle_ms"]       = *throttle_ms;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_device_options_set(const uint8_t* payload, uint16_t len,
                                         uint64_t* ieee_out,
                                         int32_t* occupancy_timeout_s_out,
                                         int32_t* debounce_ms_out,
                                         int32_t* throttle_ms_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    *ieee_out = parse_ieee(doc["ieee"] | "");
    if (*ieee_out == 0) return false;
    // Fields are optional. -1 means "not present, don't touch".
    if (occupancy_timeout_s_out) {
        *occupancy_timeout_s_out = doc["occupancy_timeout"].is<int>()
            ? doc["occupancy_timeout"].as<int32_t>() : -1;
    }
    if (debounce_ms_out) {
        *debounce_ms_out = doc["debounce_ms"].is<int>()
            ? doc["debounce_ms"].as<int32_t>() : -1;
    }
    if (throttle_ms_out) {
        *throttle_ms_out = doc["throttle_ms"].is<int>()
            ? doc["throttle_ms"].as<int32_t>() : -1;
    }
    return true;
}

bool hap_json_encode_device_options_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         bool ok) {
    JsonDocument doc;
    doc["ok"] = ok;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_device_options_ack(const uint8_t* payload, uint16_t len,
                                         bool* ok_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    if (ok_out) *ok_out = doc["ok"] | false;
    return true;
}

// ── ZIGBEE_CFG_SET / ZIGBEE_CFG_SET_ACK ─────────────────────────────────
// Operator-configured Zigbee network identity. Changing either field
// requires a subsequent factory reset to apply. S3 uses this to let
// the user pick a channel + provide a specific network key via
// Settings; P4 persists to NVS namespace `zigbee_cfg`.
//
// Request:
//   {"channel":N?, "net_key_hex":"…32-hex-chars…"?, "regenerate":bool?}
// All fields optional; absent = "don't touch". `regenerate:true` asks
// P4 to generate a fresh random key server-side (lets the operator
// refresh the key without typing one). `regenerate` overrides
// `net_key_hex` if both are supplied.
//
// Response:
//   {"ok":bool, "channel":N, "net_key_set":bool}
// `net_key_set` is always `true` after a successful set — basic is
// telling the UI "you don't need to show the stored key, just show
// that one is configured".

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hap_json_encode_zigbee_cfg_set(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     int8_t channel, const char* net_key_hex,
                                     bool regenerate) {
    JsonDocument doc;
    if (channel >= 0) doc["channel"] = channel;
    if (net_key_hex && net_key_hex[0]) doc["net_key_hex"] = net_key_hex;
    if (regenerate) doc["regenerate"] = true;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_zigbee_cfg_set(const uint8_t* payload, uint16_t len,
                                     int8_t* channel_out,
                                     uint8_t* net_key_out, size_t net_key_cap,
                                     bool* net_key_present_out,
                                     bool* regenerate_out) {
    if (channel_out) *channel_out = -1;
    if (net_key_present_out) *net_key_present_out = false;
    if (regenerate_out) *regenerate_out = false;

    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    if (channel_out && doc["channel"].is<int>()) {
        int c = doc["channel"].as<int>();
        if (c >= 11 && c <= 26) *channel_out = (int8_t)c;
    }
    if (net_key_out && net_key_cap >= 16 && doc["net_key_hex"].is<const char*>()) {
        const char* hex = doc["net_key_hex"].as<const char*>();
        if (hex && strlen(hex) == 32) {
            bool ok = true;
            for (int i = 0; i < 16; i++) {
                int hi = hex_nibble(hex[i * 2]);
                int lo = hex_nibble(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0) { ok = false; break; }
                net_key_out[i] = (uint8_t)((hi << 4) | lo);
            }
            if (ok && net_key_present_out) *net_key_present_out = true;
        }
    }
    if (regenerate_out && doc["regenerate"].is<bool>()) {
        *regenerate_out = doc["regenerate"].as<bool>();
    }
    return true;
}

bool hap_json_encode_zigbee_cfg_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     bool ok, uint8_t channel, bool net_key_set) {
    JsonDocument doc;
    doc["ok"]           = ok;
    doc["channel"]      = channel;
    doc["net_key_set"]  = net_key_set;
    size_t n = serializeJson(doc, (char*)buf, cap);
    if (n == 0 || n >= cap) return false;
    *out_len = (uint16_t)n;
    return true;
}

bool hap_json_decode_zigbee_cfg_ack(const uint8_t* payload, uint16_t len,
                                     bool* ok_out, uint8_t* channel_out,
                                     bool* net_key_set_out) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, len) != DeserializationError::Ok) return false;
    if (ok_out)          *ok_out          = doc["ok"] | false;
    if (channel_out)     *channel_out     = (uint8_t)(doc["channel"] | 0);
    if (net_key_set_out) *net_key_set_out = doc["net_key_set"] | false;
    return true;
}

// ── MQTT_MSG_IN ───────────────────────────────────────────────────────────
bool hap_json_encode_mqtt_msg_in(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const char* topic, const char* payload) {
    JsonDocument doc;
    doc["topic"]   = topic;
    doc["payload"] = payload;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_mqtt_msg_in overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_mqtt_msg_in(const uint8_t* buf, uint16_t len, HapMqttMsgIn& out) {
    JsonDocument doc;
    if (deserializeJson(doc, buf, len) != DeserializationError::Ok) return false;
    const char* t = doc["topic"]   | "";
    const char* p = doc["payload"] | "";
    strncpy(out.topic,   t, sizeof(out.topic)   - 1); out.topic[sizeof(out.topic)   - 1] = '\0';
    strncpy(out.payload, p, sizeof(out.payload) - 1); out.payload[sizeof(out.payload) - 1] = '\0';
    return true;
}

// ── TIME_SYNC ─────────────────────────────────────────────────────────────
bool hap_json_encode_time_sync(uint8_t* buf, size_t cap, uint16_t* out_len, uint32_t ts) {
    JsonDocument doc;
    doc["ts"] = ts;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_time_sync overflow"); return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_time_sync(const uint8_t* payload, uint16_t len, uint32_t* ts_out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    if (!doc["ts"].is<uint32_t>()) return false;
    *ts_out = doc["ts"].as<uint32_t>();
    return true;
}

// ── MQTT_PUBLISH ──────────────────────────────────────────────────────────
bool hap_json_encode_mqtt_publish(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const HapMqttPublish& msg) {
    JsonDocument doc;
    doc["topic"]   = msg.topic;
    doc["payload"] = msg.payload;
    doc["qos"]     = msg.qos;
    doc["retain"]  = msg.retain;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_mqtt_publish json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_mqtt_publish buf overflow"); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_mqtt_publish(const uint8_t* payload, uint16_t len, HapMqttPublish& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* t = doc["topic"]   | "";
    const char* p = doc["payload"] | "";
    strncpy(out.topic,   t, sizeof(out.topic)   - 1); out.topic[sizeof(out.topic)   - 1] = '\0';
    strncpy(out.payload, p, sizeof(out.payload) - 1); out.payload[sizeof(out.payload) - 1] = '\0';
    out.qos    = doc["qos"]    | 0u;
    out.retain = doc["retain"] | false;
    return true;
}

// ── OTA_STATUS (0x41) ─────────────────────────────────────────────────────
bool hap_json_encode_ota_status(uint8_t* buf, size_t cap, uint16_t* out_len,
                                 const HapOtaStatus& s) {
    JsonDocument doc;
    doc["ok"]    = s.ok;
    doc["rcvd"]  = s.rcvd;
    doc["total"] = s.total;
    doc["err"]   = s.err;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_ota_status overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_ota_status(const uint8_t* payload, uint16_t len,
                                 HapOtaStatus& out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    out.ok    = doc["ok"]    | false;
    out.rcvd  = doc["rcvd"]  | 0u;
    out.total = doc["total"] | 0u;
    const char* e = doc["err"] | "";
    strncpy(out.err, e, sizeof(out.err) - 1);
    out.err[sizeof(out.err) - 1] = '\0';
    return true;
}

// ── LOG_LINE (0x80) ───────────────────────────────────────────────────────
bool hap_json_encode_log_line(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const char* msg) {
    JsonDocument doc;
    doc["msg"] = msg;
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_log_line overflow");
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_decode_log_line(const uint8_t* payload, uint16_t len,
                               char* msg_out, size_t msg_max) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return false;
    const char* m = doc["msg"] | "";
    strncpy(msg_out, m, msg_max - 1);
    msg_out[msg_max - 1] = '\0';
    return true;
}

// ── BULK_STATE_UPDATE ─────────────────────────────────────────────────────
bool hap_json_encode_bulk(uint8_t* buf, size_t cap, uint16_t* out_len,
                           const HapDeviceEvent* evs, uint8_t count) {
    if (count > 50) {
        ESP_LOGE(TAG, "bulk count %d exceeds max 50", count);
        return false;
    }
    JsonDocument doc;
    JsonArray arr = doc["devs"].to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        char ieee_s[20]; fmt_ieee(ieee_s, evs[i].ieee);
        o["ieee"] = ieee_s;
        o["cl"]   = evs[i].cluster;
        o["at"]   = evs[i].attr;
        o["val"]  = evs[i].val;
        o["vt"]   = evs[i].val_type;
        o["ts"]   = evs[i].ts;
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed()) { ESP_LOGE(TAG, "encode_bulk json overflow"); return false; }
    if (n == 0 || n >= cap) { ESP_LOGE(TAG, "encode_bulk buf overflow count=%d", count); return false; }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

bool hap_json_encode_device_attr_update(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         uint64_t ieee, const char* key,
                                         uint8_t val_type, int32_t int_val,
                                         const char* str_val,
                                         uint8_t lqi, uint32_t last_seen) {
    if (!key || !buf) return false;
    JsonDocument doc;
    char ieee_s[20]; fmt_ieee(ieee_s, ieee);
    doc["type"]      = "device_update";
    doc["ieee"]      = ieee_s;
    doc["lqi"]       = lqi;
    doc["last_seen"] = last_seen;
    JsonObject attrs = doc["attrs"].to<JsonObject>();
    // val_type: ValType — 1=INT, 2=BOOL, 3=STR (see zcl_attribute.h).
    if (val_type == VAL_BOOL) {
        attrs[key] = (int_val != 0);
    } else if (val_type == VAL_STR) {
        if (str_val) attrs[key] = str_val;
        else         attrs[key] = static_cast<const char*>(nullptr);
    } else {
        // The ZHC library emits certain attributes as floats
        // (temperature/humidity in °C and %, color_x/y in [0,1]) and
        // the shadow bridge stores them as int32 scaled ×100 so the
        // NVS-backed int-only schema survives. Unscale back on the
        // JSON boundary so the UI sees natural units. Keeping the
        // key list here means the bridge + schema stay int32 and
        // only consumers that need real floats pay the /100.
        static constexpr const char* kFloatKeys[] = {
            "temperature", "humidity", "color_x", "color_y",
        };
        bool is_float = false;
        for (const char* k : kFloatKeys) {
            if (std::strcmp(key, k) == 0) { is_float = true; break; }
        }
        if (is_float) {
            attrs[key] = static_cast<float>(int_val) / 100.0f;
        } else {
            attrs[key] = int_val;
        }
    }
    size_t n = serializeJson(doc, reinterpret_cast<char*>(buf), cap);
    if (doc.overflowed() || n == 0 || n >= cap) {
        ESP_LOGE(TAG, "encode_device_attr_update overflow key=%s", key);
        return false;
    }
    *out_len = static_cast<uint16_t>(n);
    return true;
}

// ── Telegram binary pack/unpack ───────────────────────────────────────────
// Layout for SETTOKEN:    [u8 len][char token[len]]
// Layout for SETCHAT:     [u8 len][char chat[len]]
// Layout for SEND:        [u8 chat_len][char chat[chat_len]]
//                          [u8 parse_mode_len][char parse_mode[parse_mode_len]]
//                          [u16 LE text_len][char text[text_len]]
// On the wire, no NUL terminators — caller knows the length.

static bool put_u8(uint8_t* buf, size_t cap, uint16_t& pos, uint8_t v) {
    if ((size_t)pos + 1 > cap) return false;
    buf[pos++] = v; return true;
}
static bool put_bytes(uint8_t* buf, size_t cap, uint16_t& pos,
                      const char* src, uint8_t n) {
    if ((size_t)pos + n > cap) return false;
    if (n) memcpy(buf + pos, src, n);
    pos = (uint16_t)(pos + n);
    return true;
}
static bool put_u16le(uint8_t* buf, size_t cap, uint16_t& pos, uint16_t v) {
    if ((size_t)pos + 2 > cap) return false;
    buf[pos++] = (uint8_t)(v & 0xff);
    buf[pos++] = (uint8_t)(v >> 8);
    return true;
}

bool hap_pack_tg_settoken(uint8_t* buf, size_t cap, uint16_t* out_len,
                           const HapTgSettoken& m) {
    if (m.token_len > 96) return false;
    uint16_t pos = 0;
    if (!put_u8(buf, cap, pos, m.token_len)) return false;
    if (!put_bytes(buf, cap, pos, m.token, m.token_len)) return false;
    if (out_len) *out_len = pos;
    return true;
}

bool hap_unpack_tg_settoken(const uint8_t* buf, uint16_t len, HapTgSettoken* out) {
    if (!buf || !out || len < 1) return false;
    uint8_t n = buf[0];
    if (n > 96 || (uint16_t)(1 + n) > len) return false;
    out->token_len = n;
    if (n) memcpy(out->token, buf + 1, n);
    out->token[n] = '\0';
    return true;
}

bool hap_pack_tg_setchat(uint8_t* buf, size_t cap, uint16_t* out_len,
                          const HapTgSetchat& m) {
    if (m.chat_len > 32) return false;
    uint16_t pos = 0;
    if (!put_u8(buf, cap, pos, m.chat_len)) return false;
    if (!put_bytes(buf, cap, pos, m.chat, m.chat_len)) return false;
    if (out_len) *out_len = pos;
    return true;
}

bool hap_unpack_tg_setchat(const uint8_t* buf, uint16_t len, HapTgSetchat* out) {
    if (!buf || !out || len < 1) return false;
    uint8_t n = buf[0];
    if (n > 32 || (uint16_t)(1 + n) > len) return false;
    out->chat_len = n;
    if (n) memcpy(out->chat, buf + 1, n);
    out->chat[n] = '\0';
    return true;
}

bool hap_pack_tg_send(uint8_t* buf, size_t cap, uint16_t* out_len,
                       const HapTgSend& m) {
    if (m.chat_len > 32 || m.parse_mode_len > 32 || m.text_len > 3072) return false;
    uint16_t pos = 0;
    if (!put_u8(buf, cap, pos, m.chat_len)) return false;
    if (!put_bytes(buf, cap, pos, m.chat, m.chat_len)) return false;
    if (!put_u8(buf, cap, pos, m.parse_mode_len)) return false;
    if (!put_bytes(buf, cap, pos, m.parse_mode, m.parse_mode_len)) return false;
    if (!put_u16le(buf, cap, pos, m.text_len)) return false;
    if ((size_t)pos + m.text_len > cap) return false;
    if (m.text_len) memcpy(buf + pos, m.text, m.text_len);
    pos = (uint16_t)(pos + m.text_len);
    if (out_len) *out_len = pos;
    return true;
}

bool hap_unpack_tg_send(const uint8_t* buf, uint16_t len, HapTgSend* out) {
    if (!buf || !out || len < 4) return false;
    uint16_t pos = 0;
    uint8_t chat_n = buf[pos++];
    if (chat_n > 32 || (uint16_t)(pos + chat_n) > len) return false;
    if (chat_n) memcpy(out->chat, buf + pos, chat_n);
    out->chat[chat_n] = '\0';
    out->chat_len = chat_n;
    pos = (uint16_t)(pos + chat_n);
    if (pos >= len) return false;
    uint8_t pm_n = buf[pos++];
    if (pm_n > 32 || (uint16_t)(pos + pm_n) > len) return false;
    if (pm_n) memcpy(out->parse_mode, buf + pos, pm_n);
    out->parse_mode[pm_n] = '\0';
    out->parse_mode_len = pm_n;
    pos = (uint16_t)(pos + pm_n);
    if ((uint16_t)(pos + 2) > len) return false;
    uint16_t text_n = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
    pos = (uint16_t)(pos + 2);
    if (text_n > 3072 || (uint16_t)(pos + text_n) > len) return false;
    if (text_n) memcpy(out->text, buf + pos, text_n);
    out->text[text_n] = '\0';
    out->text_len = text_n;
    return true;
}
