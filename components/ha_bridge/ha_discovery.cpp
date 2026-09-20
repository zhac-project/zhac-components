// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_discovery.cpp — see ha_discovery.h. Pure: host-tested in test/host.
#include "ha_discovery.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "ArduinoJson.h"

namespace ha {
namespace {

constexpr unsigned kAccessState = 0b001;   // publishes (z2m access bits, as
constexpr unsigned kAccessSet   = 0b010;   // zhc_adapter emits them)
constexpr size_t   kPayloadCap  = 1152;
constexpr const char* kSupportUrl = "https://github.com/zhac-project/zhac-platform";

// Read-only numeric -> sensor device_class, only for the units Home Assistant
// accepts with that class (a mismatch makes HA log a warning per update).
struct SensorClass {
    const char* key;
    const char* device_class;
    const char* units;         // '|'-separated
    const char* state_class;
};
constexpr SensorClass kSensorClasses[] = {
    {"temperature",        "temperature",    "°C|°F",                   "measurement"},
    {"local_temperature",  "temperature",    "°C|°F",                   "measurement"},
    {"device_temperature", "temperature",    "°C|°F",                   "measurement"},
    {"humidity",           "humidity",       "%",                       "measurement"},
    {"soil_moisture",      "moisture",       "%",                       "measurement"},
    {"pressure",           "pressure",       "hPa|kPa|Pa|mbar|bar|psi", "measurement"},
    {"illuminance",        "illuminance",    "lx",                      "measurement"},
    {"illuminance_lux",    "illuminance",    "lx",                      "measurement"},
    {"power",              "power",          "W|kW",                    "measurement"},
    {"energy",             "energy",         "Wh|kWh|MWh",              "total_increasing"},
    {"voltage",            "voltage",        "V|mV",                    "measurement"},
    {"current",            "current",        "A|mA",                    "measurement"},
    {"battery",            "battery",        "%",                       "measurement"},
    {"co2",                "carbon_dioxide", "ppm",                     "measurement"},
    {"pm25",               "pm25",           "µg/m³",                   "measurement"},
    {"pm10",               "pm10",           "µg/m³",                   "measurement"},
};

// Read-only binary -> binary_sensor device_class. `inverted`: ZHAC follows
// zigbee2mqtt, where contact=true means CLOSED, while HA's door class reads
// "on" as OPEN.
struct BinaryClass {
    const char* key;
    const char* device_class;
    bool        inverted;
};
constexpr BinaryClass kBinaryClasses[] = {
    {"contact",         "door",            true},
    {"occupancy",       "occupancy",       false},
    {"presence",        "presence",        false},
    {"water_leak",      "moisture",        false},
    {"smoke",           "smoke",           false},
    {"gas",             "gas",             false},
    {"carbon_monoxide", "carbon_monoxide", false},
    {"tamper",          "tamper",          false},
    {"battery_low",     "battery",         false},
    {"vibration",       "vibration",       false},
};

bool unit_in(const char* units, const char* unit) {
    if (!unit || !unit[0]) return false;
    const size_t n = strlen(unit);
    for (const char* p = units; *p;) {
        const char* bar = strchr(p, '|');
        const size_t len = bar ? static_cast<size_t>(bar - p) : strlen(p);
        if (len == n && strncmp(p, unit, n) == 0) return true;
        if (!bar) break;
        p = bar + 1;
    }
    return false;
}

void humanize(const char* key, char* out, size_t cap) {
    size_t i = 0;
    for (; key[i] && i + 1 < cap; i++) out[i] = key[i] == '_' ? ' ' : key[i];
    out[i] = '\0';
    if (out[0]) out[0] = static_cast<char>(toupper(static_cast<unsigned char>(out[0])));
}

struct Expose {
    const char* name     = nullptr;
    const char* type     = nullptr;
    unsigned    access   = 0;
    const char* unit     = nullptr;
    const char* category = nullptr;
    JsonArrayConst values;
    bool has_min = false, has_max = false, has_step = false;
    long min = 0, max = 0, step = 0;
    bool used = false;

    bool is(const char* t) const { return type && strcmp(type, t) == 0; }
    bool settable() const { return (access & kAccessSet) != 0; }
    bool publishes() const { return (access & kAccessState) != 0; }
};

Expose read_expose(JsonObjectConst o) {
    Expose e;
    e.name     = o["name"] | (const char*)nullptr;
    e.type     = o["type"] | (const char*)nullptr;
    e.access   = o["access"] | 0u;
    e.unit     = o["unit"] | (const char*)nullptr;
    e.category = o["category"] | (const char*)nullptr;
    e.values   = o["values"].as<JsonArrayConst>();
    if (o["value_min"].is<long>())  { e.has_min = true;  e.min  = o["value_min"].as<long>(); }
    if (o["value_max"].is<long>())  { e.has_max = true;  e.max  = o["value_max"].as<long>(); }
    if (o["value_step"].is<long>()) { e.has_step = true; e.step = o["value_step"].as<long>(); }
    return e;
}

struct Writer {
    const Device&  d;
    const Context& c;
    EmitFn         emit;
    void*          user;
    int            count = 0;

    void topic_of(char* out, size_t cap, const char* key, bool set) const {
        const int n = state_topic(out, cap, c.root, d.ieee, key);
        if (n > 0 && set && static_cast<size_t>(n) + 4 < cap) strcat(out, "/set");
    }

    // Fields every entity shares. `key` names the entity; `name` null means
    // "the device itself" (HA then shows just the device name).
    void base(JsonDocument& doc, const char* key, const char* name) const {
        char buf[64];
        snprintf(buf, sizeof(buf), "zhac_%016" PRIx64 "_%s", d.ieee, key);
        doc["unique_id"] = buf;
        if (name) doc["name"] = name;
        else      doc["name"] = nullptr;
        snprintf(buf, sizeof(buf), "%s/availability", c.root);
        doc["availability_topic"] = buf;
        JsonObject dev = doc["device"].to<JsonObject>();
        snprintf(buf, sizeof(buf), "zhac_%016" PRIx64, d.ieee);
        dev["identifiers"].to<JsonArray>().add(buf);
        dev["name"] = (d.name && d.name[0]) ? d.name : buf;
        if (d.vendor && d.vendor[0]) dev["manufacturer"] = d.vendor;
        if (d.model && d.model[0])   dev["model"] = d.model;
        snprintf(buf, sizeof(buf), "zhac_bridge_%s", c.bridge_id);
        dev["via_device"] = buf;
        JsonObject org = doc["origin"].to<JsonObject>();
        org["name"] = "ZHAC";
        org["support_url"] = kSupportUrl;
    }

    void finish(JsonDocument& doc, const char* component, const char* key) {
        char topic[160];
        if (config_topic(topic, sizeof(topic), c.prefix, component, d.ieee, key) < 0) return;
        char payload[kPayloadCap];
        const size_t n = serializeJson(doc, payload, sizeof(payload));
        if (n == 0 || n >= sizeof(payload)) return;   // would be truncated JSON
        emit(component, topic, payload, user);
        count++;
    }

    void category(JsonDocument& doc, const Expose& e) const {
        if (!e.category) return;
        if (strcmp(e.category, "diagnostic") == 0) doc["entity_category"] = "diagnostic";
        // HA allows "config" only on entities you can change.
        else if (strcmp(e.category, "config") == 0)
            doc["entity_category"] = e.settable() ? "config" : "diagnostic";
    }

    void light(const Expose& state, const Expose& bri, const Expose* ct) {
        JsonDocument doc;
        base(doc, "light", nullptr);
        char t[160];
        topic_of(t, sizeof(t), state.name, false); doc["state_topic"] = t;
        topic_of(t, sizeof(t), state.name, true);  doc["command_topic"] = t;
        doc["payload_on"] = "1";
        doc["payload_off"] = "0";
        topic_of(t, sizeof(t), bri.name, false); doc["brightness_state_topic"] = t;
        topic_of(t, sizeof(t), bri.name, true);  doc["brightness_command_topic"] = t;
        doc["brightness_scale"] = (bri.has_max && bri.max > 0) ? bri.max : 254;
        if (ct) {
            topic_of(t, sizeof(t), ct->name, false); doc["color_temp_state_topic"] = t;
            topic_of(t, sizeof(t), ct->name, true);  doc["color_temp_command_topic"] = t;
            if (ct->has_min) doc["min_mireds"] = ct->min;
            if (ct->has_max) doc["max_mireds"] = ct->max;
        }
        finish(doc, "light", "light");
    }

    void one(const Expose& e) {
        const bool set = e.settable();
        char label[48];
        humanize(e.name, label, sizeof(label));
        // A device whose main job is "state" (a plug, a relay) is that entity.
        const char* name = strcmp(e.name, "state") == 0 ? nullptr : label;
        JsonDocument doc;
        base(doc, e.name, name);
        char t[160];
        topic_of(t, sizeof(t), e.name, false);
        doc["state_topic"] = t;
        if (set) { topic_of(t, sizeof(t), e.name, true); doc["command_topic"] = t; }
        category(doc, e);

        if (e.is("binary")) {
            if (set) {
                doc["payload_on"] = "1";  doc["payload_off"] = "0";
                doc["state_on"]   = "1";  doc["state_off"]   = "0";
                return finish(doc, "switch", e.name);
            }
            bool inverted = false;
            for (const auto& b : kBinaryClasses) {
                if (strcmp(b.key, e.name) == 0) {
                    doc["device_class"] = b.device_class;
                    inverted = b.inverted;
                    break;
                }
            }
            doc["payload_on"]  = inverted ? "0" : "1";
            doc["payload_off"] = inverted ? "1" : "0";
            return finish(doc, "binary_sensor", e.name);
        }
        if (e.is("numeric")) {
            if (e.unit && e.unit[0]) doc["unit_of_measurement"] = e.unit;
            if (set) {
                if (e.has_min && e.has_max && e.max > e.min) {
                    doc["min"] = e.min;
                    doc["max"] = e.max;
                } else {                      // range unknown: don't let HA's
                    doc["min"] = -1000000;    // default 1..100 reject values
                    doc["max"] = 1000000;
                    doc["mode"] = "box";
                }
                if (e.has_step && e.step > 0) doc["step"] = e.step;
                return finish(doc, "number", e.name);
            }
            const char* sc = "measurement";
            for (const auto& s : kSensorClasses) {
                if (strcmp(s.key, e.name) == 0 && unit_in(s.units, e.unit)) {
                    doc["device_class"] = s.device_class;
                    sc = s.state_class;
                    break;
                }
            }
            doc["state_class"] = sc;
            return finish(doc, "sensor", e.name);
        }
        if (e.is("enum")) {
            if (set && !e.values.isNull() && e.values.size() > 0) {
                JsonArray opts = doc["options"].to<JsonArray>();
                for (JsonVariantConst v : e.values) opts.add(v.as<const char*>());
                return finish(doc, "select", e.name);
            }
            return finish(doc, "sensor", e.name);
        }
        if (e.is("text")) return finish(doc, set ? "text" : "sensor", e.name);
    }
};

}  // namespace

int build_device(const Device& d, const Context& c, EmitFn emit, void* user) {
    JsonDocument in;
    if (!d.exposes || deserializeJson(in, d.exposes) || !in.is<JsonArrayConst>()) return -1;
    JsonArrayConst arr = in.as<JsonArrayConst>();

    constexpr size_t kMax = 64;
    std::unique_ptr<Expose[]> ex(new (std::nothrow) Expose[kMax]);   // ~5 KB: off the stack
    if (!ex) return -1;
    size_t n = 0;
    for (JsonObjectConst o : arr) {
        if (n == kMax) break;
        Expose e = read_expose(o);
        if (!e.name || !e.name[0] || !e.type) continue;
        ex[n++] = e;
    }
    auto find = [&](const char* name, const char* type) -> Expose* {
        for (size_t i = 0; i < n; i++)
            if (strcmp(ex[i].name, name) == 0 && ex[i].is(type)) return &ex[i];
        return nullptr;
    };

    Writer w{d, c, emit, user};
    // state + brightness (+ color_temp), all writable, is a dimmable light.
    Expose* st = find("state", "binary");
    Expose* br = find("brightness", "numeric");
    if (st && br && st->settable() && br->settable()) {
        Expose* ct = find("color_temp", "numeric");
        if (ct && !ct->settable()) ct = nullptr;
        w.light(*st, *br, ct);
        st->used = br->used = true;
        if (ct) ct->used = true;
    }
    for (size_t i = 0; i < n; i++) {
        // Write-only exposes (identify, effect triggers) have no state to show;
        // they stay on the web UI's Commands tab.
        if (ex[i].used || !ex[i].publishes()) continue;
        w.one(ex[i]);
    }
    return w.count;
}

void build_bridge(const Context& c, const char* fw_version, const char* model,
                  EmitFn emit, void* user) {
    JsonDocument doc;
    char buf[96];
    doc["name"] = "Connection";
    snprintf(buf, sizeof(buf), "zhac_bridge_%s_connection", c.bridge_id);
    doc["unique_id"] = buf;
    snprintf(buf, sizeof(buf), "%s/availability", c.root);
    doc["state_topic"] = buf;
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    doc["device_class"] = "connectivity";
    doc["entity_category"] = "diagnostic";
    JsonObject dev = doc["device"].to<JsonObject>();
    snprintf(buf, sizeof(buf), "zhac_bridge_%s", c.bridge_id);
    dev["identifiers"].to<JsonArray>().add(buf);
    if (strcmp(c.bridge_id, "zhac") == 0) dev["name"] = "ZHAC hub";
    else { snprintf(buf, sizeof(buf), "ZHAC hub %s", c.bridge_id); dev["name"] = buf; }
    dev["manufacturer"] = "ZHAC";
    if (model && model[0]) dev["model"] = model;
    if (fw_version && fw_version[0]) dev["sw_version"] = fw_version;
    JsonObject org = doc["origin"].to<JsonObject>();
    org["name"] = "ZHAC";
    org["support_url"] = kSupportUrl;

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/binary_sensor/zhac_bridge_%s_connection/config",
             c.prefix, c.bridge_id);
    char payload[kPayloadCap];
    const size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n > 0 && n < sizeof(payload)) emit("binary_sensor", topic, payload, user);
}

int state_topic(char* out, size_t cap, const char* root, uint64_t ieee, const char* key) {
    const int n = snprintf(out, cap, "%s/devices/%016" PRIX64 "/%s", root, ieee, key);
    return (n < 0 || static_cast<size_t>(n) >= cap) ? -1 : n;
}

int config_topic(char* out, size_t cap, const char* prefix, const char* component,
                 uint64_t ieee, const char* key) {
    const int n = snprintf(out, cap, "%s/%s/zhac_%016" PRIx64 "_%s/config",
                           prefix, component, ieee, key);
    return (n < 0 || static_cast<size_t>(n) >= cap) ? -1 : n;
}

bool parse_command_topic(const char* topic, size_t len, const char* root,
                         uint64_t* ieee, char* key, size_t key_cap) {
    const size_t rlen = strlen(root);
    static constexpr char kDev[] = "/devices/";
    static constexpr char kSet[] = "/set";
    const size_t dlen = sizeof(kDev) - 1, slen = sizeof(kSet) - 1;
    // root + "/devices/" + 16 hex + "/" + key(>=1) + "/set"
    if (len < rlen + dlen + 16 + 1 + 1 + slen) return false;
    if (strncmp(topic, root, rlen) != 0 || strncmp(topic + rlen, kDev, dlen) != 0) return false;
    if (strncmp(topic + len - slen, kSet, slen) != 0) return false;
    const char* hex = topic + rlen + dlen;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const char ch = hex[i];
        int nib;
        if (ch >= '0' && ch <= '9')      nib = ch - '0';
        else if (ch >= 'a' && ch <= 'f') nib = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') nib = ch - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(nib);
    }
    if (hex[16] != '/') return false;
    const char* k = hex + 17;
    const size_t klen = static_cast<size_t>(topic + len - slen - k);
    if (klen == 0 || klen >= key_cap) return false;
    for (size_t i = 0; i < klen; i++) {
        const char ch = k[i];
        if (!(islower(static_cast<unsigned char>(ch)) || isdigit(static_cast<unsigned char>(ch)) || ch == '_'))
            return false;                 // also rejects a nested '/'
    }
    memcpy(key, k, klen);
    key[klen] = '\0';
    *ieee = v;
    return v != 0;
}

bool command_value_json(const char* payload, size_t len, char* out, size_t cap) {
    while (len && isspace(static_cast<unsigned char>(payload[0]))) { payload++; len--; }
    while (len && isspace(static_cast<unsigned char>(payload[len - 1]))) len--;
    // A plain decimal number: pass through, dropping an all-zero fraction so
    // "21.0" reaches the integer-only attribute setters as 21.
    size_t i = (len && payload[0] == '-') ? 1 : 0;
    size_t digits = 0, dot = 0, frac_nonzero = 0;
    bool numeric = len > i;
    for (; i < len && numeric; i++) {
        const char ch = payload[i];
        if (isdigit(static_cast<unsigned char>(ch))) { digits++; if (dot && ch != '0') frac_nonzero++; }
        else if (ch == '.' && !dot) dot = i;
        else numeric = false;
    }
    if (numeric && digits) {
        const size_t keep = (dot && !frac_nonzero) ? dot : len;
        if (keep + 1 > cap) return false;
        memcpy(out, payload, keep);
        out[keep] = '\0';
        return true;
    }
    if ((len == 4 && strncmp(payload, "true", 4) == 0) ||
        (len == 5 && strncmp(payload, "false", 5) == 0)) {
        if (len + 1 > cap) return false;
        memcpy(out, payload, len);
        out[len] = '\0';
        return true;
    }
    size_t o = 0;                          // anything else is a string
    if (cap < 3) return false;
    out[o++] = '"';
    for (size_t j = 0; j < len; j++) {
        const unsigned char ch = static_cast<unsigned char>(payload[j]);
        if (ch < 0x20) continue;
        if (ch == '"' || ch == '\\') { if (o + 2 >= cap) return false; out[o++] = '\\'; }
        if (o + 2 >= cap) return false;
        out[o++] = static_cast<char>(ch);
    }
    out[o++] = '"';
    out[o] = '\0';
    return true;
}

bool state_payload(const char* value_json, char* out, size_t cap) {
    if (!value_json || cap < 2) return false;
    JsonDocument doc;
    if (deserializeJson(doc, value_json) || doc.isNull()) return false;
    if (doc.is<bool>()) {
        out[0] = doc.as<bool>() ? '1' : '0';
        out[1] = '\0';
        return true;
    }
    if (doc.is<const char*>()) {
        const char* s = doc.as<const char*>();
        const size_t n = strlen(s);
        if (n + 1 > cap) return false;
        memcpy(out, s, n + 1);
        return true;
    }
    if (doc.is<JsonObjectConst>() || doc.is<JsonArrayConst>()) return false;
    const size_t n = strlen(value_json);    // a number: keep the caller's text
    if (n + 1 > cap) return false;
    memcpy(out, value_json, n + 1);
    return true;
}

int x100_to_json(char* out, size_t cap, int32_t v) {
    const bool neg = v < 0;
    const uint32_t a = neg ? static_cast<uint32_t>(-(int64_t)v) : static_cast<uint32_t>(v);
    const uint32_t whole = a / 100, frac = a % 100;
    int n;
    if (frac == 0)           n = snprintf(out, cap, "%s%lu", neg ? "-" : "", (unsigned long)whole);
    else if (frac % 10 == 0) n = snprintf(out, cap, "%s%lu.%lu", neg ? "-" : "", (unsigned long)whole, (unsigned long)(frac / 10));
    else                     n = snprintf(out, cap, "%s%lu.%02lu", neg ? "-" : "", (unsigned long)whole, (unsigned long)frac);
    return (n < 0 || static_cast<size_t>(n) >= cap) ? -1 : n;
}

}  // namespace ha
