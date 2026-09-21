// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Pins what ZHAC tells Home Assistant: which entity each expose becomes, the
// topics it uses, and the payload conversions both directions.
#include "ha_discovery.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "ArduinoJson.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

struct Entity { std::string component, topic; JsonDocument cfg; };
static std::map<std::string, Entity> g_out;   // keyed by unique_id

static void collect(const char* component, const char* topic, const char* payload, void*) {
    Entity e;
    e.component = component;
    e.topic = topic;
    if (deserializeJson(e.cfg, payload)) { std::printf("FAIL: invalid JSON for %s\n", topic); g_fail++; return; }
    const std::string uid = e.cfg["unique_id"].as<const char*>();
    g_out[uid] = std::move(e);
}

static const ha::Context kCtx{"homeassistant", "zhac", "zhac"};

static bool g_passive = false;
static int build(const char* name, const char* vendor, const char* model, const char* exposes,
                 uint64_t ieee = 0x00158D0007A1B2C3ULL, bool battery = false) {
    g_out.clear();
    return ha::build_device({ieee, name, vendor, model, exposes, battery}, kCtx, collect, nullptr, &g_passive);
}
static bool has(JsonArrayConst a, const char* v) {
    for (JsonVariantConst x : a) if (x.is<const char*>() && std::string(x.as<const char*>()) == v) return true;
    return false;
}

static const Entity* get(const char* uid) {
    auto it = g_out.find(uid);
    return it == g_out.end() ? nullptr : &it->second;
}

static void test_sensors_and_binary() {
    const char* exposes = R"([
        {"name":"occupancy","type":"binary","access":1},
        {"name":"contact","type":"binary","access":1},
        {"name":"temperature","type":"numeric","access":1,"unit":"°C"},
        {"name":"battery","type":"numeric","access":1,"unit":"%","category":"diagnostic"},
        {"name":"illuminance","type":"numeric","access":1,"unit":"lux"}
    ])";
    CHECK(build("hallway_motion", "Aqara", "RTCGQ11LM", exposes) == 5);

    const Entity* occ = get("zhac_00158d0007a1b2c3_occupancy");
    CHECK(occ && occ->component == "binary_sensor");
    CHECK(occ && occ->topic == "homeassistant/binary_sensor/zhac_00158d0007a1b2c3_occupancy/config");
    CHECK(occ && std::string(occ->cfg["state_topic"]) == "zhac/devices/00158D0007A1B2C3/occupancy");
    CHECK(occ && std::string(occ->cfg["device_class"]) == "occupancy");
    CHECK(occ && std::string(occ->cfg["payload_on"]) == "1");
    // has a battery expose: hub topic AND its own availability topic
    CHECK(g_passive);
    CHECK(occ && occ->cfg["availability_topic"].isNull());
    CHECK(occ && std::string(occ->cfg["availability"][0]["topic"]) == "zhac/availability");
    CHECK(occ && std::string(occ->cfg["availability"][1]["topic"]) == "zhac/devices/00158D0007A1B2C3/availability");
    CHECK(occ && std::string(occ->cfg["availability_mode"]) == "all");
    CHECK(occ && std::string(occ->cfg["name"]) == "Occupancy");
    CHECK(occ && !occ->cfg["command_topic"].is<const char*>());
    CHECK(occ && std::string(occ->cfg["device"]["identifiers"][0]) == "zhac_00158d0007a1b2c3");
    CHECK(occ && std::string(occ->cfg["device"]["name"]) == "hallway_motion");
    CHECK(occ && std::string(occ->cfg["device"]["manufacturer"]) == "Aqara");
    CHECK(occ && std::string(occ->cfg["device"]["model"]) == "RTCGQ11LM");
    CHECK(occ && std::string(occ->cfg["device"]["via_device"]) == "zhac_bridge_zhac");

    // contact=true is CLOSED (zigbee2mqtt semantics); HA's door is on=open.
    const Entity* con = get("zhac_00158d0007a1b2c3_contact");
    CHECK(con && std::string(con->cfg["device_class"]) == "door");
    CHECK(con && std::string(con->cfg["payload_on"]) == "0");
    CHECK(con && std::string(con->cfg["payload_off"]) == "1");

    const Entity* t = get("zhac_00158d0007a1b2c3_temperature");
    CHECK(t && t->component == "sensor");
    CHECK(t && std::string(t->cfg["device_class"]) == "temperature");
    CHECK(t && std::string(t->cfg["unit_of_measurement"]) == "°C");
    CHECK(t && std::string(t->cfg["state_class"]) == "measurement");

    const Entity* b = get("zhac_00158d0007a1b2c3_battery");
    CHECK(b && std::string(b->cfg["entity_category"]) == "diagnostic");
    CHECK(b && std::string(b->cfg["device_class"]) == "battery");

    // A unit HA does not accept for the class: keep the unit, drop the class.
    const Entity* lux = get("zhac_00158d0007a1b2c3_illuminance");
    CHECK(lux && lux->cfg["device_class"].isNull());
    CHECK(lux && std::string(lux->cfg["unit_of_measurement"]) == "lux");
}

static void test_light() {
    const char* exposes = R"([
        {"name":"state","type":"binary","access":3},
        {"name":"brightness","type":"numeric","access":3,"value_min":0,"value_max":254},
        {"name":"color_temp","type":"numeric","access":3,"unit":"mired","value_min":250,"value_max":454}
    ])";
    CHECK(build("living_room_lamp", "IKEA", "LED1836G9", exposes, 0x000B57FFFE8C4D21ULL) == 1);
    const Entity* l = get("zhac_000b57fffe8c4d21_light");
    CHECK(l && l->component == "light");
    CHECK(l && l->cfg["name"].isNull());   // the device's own name
    CHECK(l && std::string(l->cfg["command_topic"]) == "zhac/devices/000B57FFFE8C4D21/state/set");
    CHECK(l && std::string(l->cfg["brightness_command_topic"]) == "zhac/devices/000B57FFFE8C4D21/brightness/set");
    CHECK(l && l->cfg["brightness_scale"].as<int>() == 254);
    CHECK(l && std::string(l->cfg["color_temp_state_topic"]) == "zhac/devices/000B57FFFE8C4D21/color_temp");
    CHECK(l && l->cfg["min_mireds"].as<int>() == 250);
    CHECK(l && l->cfg["max_mireds"].as<int>() == 454);
    CHECK(l && l->cfg["xy_command_topic"].isNull() && l->cfg["hs_command_topic"].isNull());

    // colour: xy from color_x/color_y, hs from hue/saturation; the axes are consumed
    const char* rgb = R"([
        {"name":"state","type":"binary","access":3},
        {"name":"brightness","type":"numeric","access":3},
        {"name":"color_x","type":"numeric","access":3},
        {"name":"color_y","type":"numeric","access":3},
        {"name":"hue","type":"numeric","access":3},
        {"name":"saturation","type":"numeric","access":3},
        {"name":"color_mode","type":"enum","access":1,"values":["hs","xy","color_temp"]}
    ])";
    CHECK(build("rgb_bulb", "", "", rgb) == 2);   // light + color_mode sensor
    l = get("zhac_00158d0007a1b2c3_light");
    CHECK(l && std::string(l->cfg["xy_state_topic"]) == "zhac/devices/00158D0007A1B2C3/color_xy");
    CHECK(l && std::string(l->cfg["xy_command_topic"]) == "zhac/devices/00158D0007A1B2C3/color_xy/set");
    CHECK(l && std::string(l->cfg["hs_command_topic"]) == "zhac/devices/00158D0007A1B2C3/color_hs/set");
    CHECK(!get("zhac_00158d0007a1b2c3_color_x") && !get("zhac_00158d0007a1b2c3_hue"));
    // a definition that names the pair itself, whatever type it gave it
    CHECK(build("bulb", "", "", R"([{"name":"state","type":"binary","access":3},{"name":"brightness","type":"numeric","access":3},{"name":"color_xy","type":"numeric","access":3}])") == 1);
    l = get("zhac_00158d0007a1b2c3_light");
    CHECK(l && l->cfg["xy_command_topic"].is<const char*>() && l->cfg["hs_command_topic"].isNull());
}

static void test_plug_select_number_and_skips() {
    const char* exposes = R"([
        {"name":"state","type":"binary","access":3},
        {"name":"power","type":"numeric","access":1,"unit":"W"},
        {"name":"energy","type":"numeric","access":1,"unit":"kWh"},
        {"name":"power_outage_memory","type":"enum","access":3,"category":"config","values":["on","off","restore"]},
        {"name":"current_heating_setpoint","type":"numeric","access":3,"unit":"°C","value_min":5,"value_max":35,"value_step":1},
        {"name":"calibration","type":"numeric","access":3},
        {"name":"identify","type":"enum","access":2,"values":["identify"]},
        {"name":"action","type":"enum","access":1,"values":["single","double"]},
        {"name":"child_lock","type":"binary","access":3,"category":"config"},
        {"type":"numeric","access":1},
        {"name":"note","type":"text","access":1}
    ])";
    // 9 published: identify is write-only, the nameless expose is skipped.
    CHECK(build("kitchen_plug", "Tuya", "TS011F_plug_1", exposes) == 9);

    const Entity* sw = get("zhac_00158d0007a1b2c3_state");
    CHECK(sw && sw->component == "switch");
    CHECK(sw && sw->cfg["name"].isNull());
    CHECK(sw && std::string(sw->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/state/set");
    CHECK(sw && std::string(sw->cfg["payload_on"]) == "1");

    const Entity* en = get("zhac_00158d0007a1b2c3_energy");
    CHECK(en && std::string(en->cfg["state_class"]) == "total_increasing");
    CHECK(en && std::string(en->cfg["device_class"]) == "energy");

    const Entity* sel = get("zhac_00158d0007a1b2c3_power_outage_memory");
    CHECK(sel && sel->component == "select");
    CHECK(sel && sel->cfg["options"].size() == 3);
    CHECK(sel && std::string(sel->cfg["entity_category"]) == "config");
    CHECK(sel && std::string(sel->cfg["name"]) == "Power outage memory");

    const Entity* sp = get("zhac_00158d0007a1b2c3_current_heating_setpoint");
    CHECK(sp && sp->component == "number");
    CHECK(sp && sp->cfg["min"].as<int>() == 5 && sp->cfg["max"].as<int>() == 35);
    CHECK(sp && sp->cfg["step"].as<int>() == 1);

    const Entity* cal = get("zhac_00158d0007a1b2c3_calibration");   // no range known
    CHECK(cal && std::string(cal->cfg["mode"]) == "box");
    CHECK(cal && cal->cfg["min"].as<int>() == -1000000);

    const Entity* act = get("zhac_00158d0007a1b2c3_action");
    CHECK(act && act->component == "event");
    CHECK(act && act->cfg["event_types"].size() == 2 && has(act->cfg["event_types"], "double"));
    CHECK(act && std::string(act->cfg["value_template"]) == "{\"event_type\":\"{{ value }}\"}");
    // mains device: one hub availability topic, no per-device one
    CHECK(sw && std::string(sw->cfg["availability_topic"]) == "zhac/availability");
    CHECK(sw && sw->cfg["availability"].isNull());
    CHECK(!g_passive);
    CHECK(!get("zhac_00158d0007a1b2c3_identify"));

    const Entity* cl = get("zhac_00158d0007a1b2c3_child_lock");
    CHECK(cl && cl->component == "switch");
    CHECK(cl && std::string(cl->cfg["entity_category"]) == "config");

    const Entity* note = get("zhac_00158d0007a1b2c3_note");
    CHECK(note && note->component == "sensor");
}

static void test_bad_input_and_bridge() {
    CHECK(build("x", "", "", "not json") == -1);
    CHECK(build("x", "", "", R"({"name":"state"})") == -1);
    CHECK(build("x", "", "", "[]") == 0);

    g_out.clear();
    ha::build_bridge({"homeassistant", "home/zhac-garage", "home_zhac-garage"}, "v2026091801", "ESP32-P4 wired",
                     collect, nullptr);
    const Entity* br = get("zhac_bridge_home_zhac-garage_connection");
    CHECK(br && br->topic == "homeassistant/binary_sensor/zhac_bridge_home_zhac-garage_connection/config");
    CHECK(br && std::string(br->cfg["state_topic"]) == "home/zhac-garage/availability");
    CHECK(br && std::string(br->cfg["device_class"]) == "connectivity");
    CHECK(br && std::string(br->cfg["device"]["identifiers"][0]) == "zhac_bridge_home_zhac-garage");
    CHECK(br && std::string(br->cfg["device"]["sw_version"]) == "v2026091801");
}

static void test_topics_and_payloads() {
    uint64_t ieee = 0;
    char key[32];
    const char* t = "zhac/devices/000B57FFFE8C4D21/brightness/set";
    CHECK(ha::parse_command_topic(t, strlen(t), "zhac", &ieee, key, sizeof(key)));
    CHECK(ieee == 0x000B57FFFE8C4D21ULL && std::string(key) == "brightness");
    const char* lower = "zhac/devices/000b57fffe8c4d21/state/set";
    CHECK(ha::parse_command_topic(lower, strlen(lower), "zhac", &ieee, key, sizeof(key)));
    const char* not_cmd[] = {
        "zhac/devices/000B57FFFE8C4D21/brightness",      // a state topic
        "zhac/devices/000B57FFFE8C4D21/state",
        "other/devices/000B57FFFE8C4D21/state/set",      // another hub's root
        "zhac/devices/000B57FFFE8C4D2/state/set",        // 15 hex digits
        "zhac/devices/000B57FFFE8C4D21/a/b/set",         // nested key
        "zhac/devices/000B57FFFE8C4D21//set",
        "zhac/devices/0000000000000000/state/set",       // ieee 0
        "zhac/devices/000B57FFFE8C4D21/State/set",       // keys are snake_case
    };
    for (const char* s : not_cmd) CHECK(!ha::parse_command_topic(s, strlen(s), "zhac", &ieee, key, sizeof(key)));
    const char* longkey = "zhac/devices/000B57FFFE8C4D21/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/set";
    CHECK(!ha::parse_command_topic(longkey, strlen(longkey), "zhac", &ieee, key, sizeof(key)));

    char out[64];
    auto cmd = [&](const char* in) { return ha::command_value_json(in, strlen(in), out, sizeof(out)) ? std::string(out) : std::string("<fail>"); };
    CHECK(cmd("1") == "1");
    CHECK(cmd("21.0") == "21");
    CHECK(cmd("21.50") == "21.50");
    CHECK(cmd("-3") == "-3");
    CHECK(cmd(" 254 ") == "254");
    CHECK(cmd("true") == "true");
    CHECK(cmd("heat") == "\"heat\"");
    CHECK(cmd("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(cmd("1.2.3") == "\"1.2.3\"");
    CHECK(cmd("-") == "\"-\"");

    auto st = [&](const char* in) { return ha::state_payload(in, out, sizeof(out)) ? std::string(out) : std::string("<fail>"); };
    CHECK(st("true") == "1");
    CHECK(st("false") == "0");
    CHECK(st("21.37") == "21.37");
    CHECK(st("\"restore\"") == "restore");
    CHECK(st("null") == "<fail>");
    CHECK(st("{\"a\":1}") == "<fail>");

    auto x = [&](int32_t v) { return ha::x100_to_json(out, sizeof(out), v) > 0 ? std::string(out) : std::string("<fail>"); };
    CHECK(x(2137) == "21.37");
    CHECK(x(4820) == "48.2");
    CHECK(x(2100) == "21");
    CHECK(x(0) == "0");
    CHECK(x(-5) == "-0.05");
    CHECK(x(-150) == "-1.5");
    CHECK(x(INT32_MIN) == "-21474836.48");
    CHECK(ha::x100_to_json(out, 3, 2137) == -1);

    char topic[80];
    CHECK(ha::state_topic(topic, sizeof(topic), "zhac", 0xA4C138F3E2D10B77ULL, "power") > 0);
    CHECK(std::string(topic) == "zhac/devices/A4C138F3E2D10B77/power");
    CHECK(ha::state_topic(topic, 10, "zhac", 1, "power") == -1);
}

static void test_climate() {
    const char* exposes = R"([
        {"name":"local_temperature","type":"numeric","access":1,"unit":"°C"},
        {"name":"current_heating_setpoint","type":"numeric","access":3,"unit":"°C","value_min":5,"value_max":35,"value_step":1},
        {"name":"system_mode","type":"enum","access":3,"values":["off","heat","auto","emergency_heating"]},
        {"name":"preset","type":"enum","access":3,"values":["none","manual","schedule"]},
        {"name":"running_state","type":"enum","access":1,"values":["idle","heat"]},
        {"name":"fan_mode","type":"enum","access":3,"values":["low","high","auto"]},
        {"name":"local_temperature_calibration","type":"numeric","access":3,"category":"config","value_min":-5,"value_max":5},
        {"name":"child_lock","type":"binary","access":3,"category":"config"},
        {"name":"battery","type":"numeric","access":1,"unit":"%","category":"diagnostic"}
    ])";
    // climate + calibration + child_lock + battery; the six climate parts are consumed
    CHECK(build("bedroom_trv", "Tuya", "TS0601_thermostat", exposes) == 4);
    const Entity* c = get("zhac_00158d0007a1b2c3_climate");
    CHECK(c && c->component == "climate");
    CHECK(c && c->cfg["name"].isNull());
    CHECK(c && std::string(c->cfg["temperature_command_topic"]) == "zhac/devices/00158D0007A1B2C3/current_heating_setpoint/set");
    CHECK(c && std::string(c->cfg["current_temperature_topic"]) == "zhac/devices/00158D0007A1B2C3/local_temperature");
    CHECK(c && std::string(c->cfg["temperature_unit"]) == "C");
    CHECK(c && c->cfg["min_temp"].as<int>() == 5 && c->cfg["max_temp"].as<int>() == 35 && c->cfg["temp_step"].as<int>() == 1);
    CHECK(c && c->cfg["modes"].size() == 3 && has(c->cfg["modes"], "off") && has(c->cfg["modes"], "heat") && has(c->cfg["modes"], "auto"));
    CHECK(c && !has(c->cfg["modes"], "emergency_heating"));   // not a Home Assistant hvac mode
    CHECK(c && std::string(c->cfg["mode_command_topic"]) == "zhac/devices/00158D0007A1B2C3/system_mode/set");
    CHECK(c && c->cfg["preset_modes"].size() == 2 && !has(c->cfg["preset_modes"], "none"));
    CHECK(c && std::string(c->cfg["preset_mode_state_topic"]) == "zhac/devices/00158D0007A1B2C3/preset");
    CHECK(c && std::string(c->cfg["action_topic"]) == "zhac/devices/00158D0007A1B2C3/running_state");
    CHECK(c && std::string(c->cfg["action_template"]).find("'heat':'heating'") != std::string::npos);
    CHECK(c && c->cfg["fan_modes"].size() == 3 && std::string(c->cfg["fan_mode_command_topic"]) == "zhac/devices/00158D0007A1B2C3/fan_mode/set");
    // battery device: hub topic AND its own availability topic, both required
    CHECK(g_passive);
    CHECK(c && c->cfg["availability"].size() == 2 && std::string(c->cfg["availability_mode"]) == "all");
    CHECK(c && std::string(c->cfg["availability"][1]["topic"]) == "zhac/devices/00158D0007A1B2C3/availability");
    CHECK(c && c->cfg["availability_topic"].isNull());
    CHECK(!get("zhac_00158d0007a1b2c3_system_mode") && !get("zhac_00158d0007a1b2c3_fan_mode"));
    CHECK(get("zhac_00158d0007a1b2c3_local_temperature_calibration") && get("zhac_00158d0007a1b2c3_child_lock"));

    // no mode switch, binary running_state, setpoint in Fahrenheit
    const char* heater = R"([
        {"name":"local_temperature","type":"numeric","access":1},
        {"name":"occupied_heating_setpoint","type":"numeric","access":3,"unit":"°F"},
        {"name":"running_state","type":"binary","access":1}
    ])";
    CHECK(build("heater", "", "", heater) == 1);
    c = get("zhac_00158d0007a1b2c3_climate");
    CHECK(c && c->cfg["modes"].size() == 1 && has(c->cfg["modes"], "heat") && c->cfg["mode_command_topic"].isNull());
    CHECK(c && std::string(c->cfg["temperature_unit"]) == "F");
    CHECK(c && std::string(c->cfg["action_template"]).find("'1':'heating'") != std::string::npos);
    CHECK(!g_passive);
}

static void test_cover_lock_fan() {
    const char* cover = R"([
        {"name":"state","type":"enum","access":3,"values":["OPEN","CLOSE","STOP"]},
        {"name":"position","type":"numeric","access":3,"unit":"%","value_min":0,"value_max":100},
        {"name":"tilt","type":"numeric","access":3,"value_min":0,"value_max":100},
        {"name":"moving","type":"enum","access":1,"values":["UP","DOWN","STOP"]}
    ])";
    CHECK(build("blind", "", "", cover) == 2);   // cover + moving sensor
    const Entity* cv = get("zhac_00158d0007a1b2c3_cover");
    CHECK(cv && cv->component == "cover");
    CHECK(cv && std::string(cv->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/state/set");
    CHECK(cv && std::string(cv->cfg["payload_stop"]) == "STOP" && std::string(cv->cfg["state_closed"]) == "CLOSE");
    CHECK(cv && std::string(cv->cfg["set_position_topic"]) == "zhac/devices/00158D0007A1B2C3/position/set");
    CHECK(cv && cv->cfg["position_open"].as<int>() == 100 && cv->cfg["position_closed"].as<int>() == 0);
    CHECK(cv && std::string(cv->cfg["tilt_command_topic"]) == "zhac/devices/00158D0007A1B2C3/tilt/set");
    CHECK(get("zhac_00158d0007a1b2c3_moving") && get("zhac_00158d0007a1b2c3_moving")->component == "sensor");
    // position only, and a state enum without STOP
    CHECK(build("blind", "", "", R"([{"name":"position","type":"numeric","access":3,"unit":"%"}])") == 1);
    cv = get("zhac_00158d0007a1b2c3_cover");
    CHECK(cv && cv->cfg["command_topic"].isNull() && cv->cfg["position_topic"].is<const char*>());
    CHECK(build("blind", "", "", R"([{"name":"state","type":"enum","access":3,"values":["OPEN","CLOSE"]}])") == 1);
    cv = get("zhac_00158d0007a1b2c3_cover");
    CHECK(cv && cv->cfg["payload_stop"].isNull() && std::string(cv->cfg["payload_open"]) == "OPEN");
    // a select named state that is not a cover stays a select
    CHECK(build("x", "", "", R"([{"name":"state","type":"enum","access":3,"values":["a","b"]}])") == 1);
    CHECK(get("zhac_00158d0007a1b2c3_state") && get("zhac_00158d0007a1b2c3_state")->component == "select");

    const char* lock = R"([
        {"name":"lock_state","type":"binary","access":3},
        {"name":"battery","type":"numeric","access":1,"unit":"%"}
    ])";
    CHECK(build("front_door", "Yale", "YRD426", lock, 0x00158D0007A1B2C3ULL, true) == 2);
    const Entity* lk = get("zhac_00158d0007a1b2c3_lock");
    CHECK(lk && lk->component == "lock");
    CHECK(lk && std::string(lk->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/lock_state/set");
    CHECK(lk && std::string(lk->cfg["payload_lock"]) == "1" && std::string(lk->cfg["state_unlocked"]) == "0");
    CHECK(g_passive && lk->cfg["availability"].size() == 2);
    // the zigbee2mqtt shape: LOCK/UNLOCK commands on `state`, the word on `lock_state`
    const char* worded = R"([
        {"name":"state","type":"enum","access":2,"values":["LOCK","UNLOCK"]},
        {"name":"lock_state","type":"enum","access":1,"values":["not_fully_locked","locked","unlocked"]},
        {"name":"action","type":"enum","access":1,"values":["lock","unlock","auto_lock"]}
    ])";
    CHECK(build("front_door", "Kwikset", "99140-002", worded) == 2);   // lock + action event
    lk = get("zhac_00158d0007a1b2c3_lock");
    CHECK(lk && std::string(lk->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/state/set");
    CHECK(lk && std::string(lk->cfg["state_topic"]) == "zhac/devices/00158D0007A1B2C3/lock_state");
    CHECK(lk && std::string(lk->cfg["payload_lock"]) == "LOCK" && std::string(lk->cfg["state_unlocked"]) == "unlocked");
    CHECK(!get("zhac_00158d0007a1b2c3_state") && !get("zhac_00158d0007a1b2c3_lock_state"));
    // a worded lock_state with no command row stays a sensor
    CHECK(build("x", "", "", R"([{"name":"lock_state","type":"enum","access":1,"values":["locked","unlocked"]}])") == 1);
    CHECK(get("zhac_00158d0007a1b2c3_lock_state") && get("zhac_00158d0007a1b2c3_lock_state")->component == "sensor");

    // fan with its own on/off and a speed list
    const char* fan1 = R"([
        {"name":"fan_state","type":"binary","access":3},
        {"name":"fan_mode","type":"enum","access":3,"values":["low","medium","high","auto"]}
    ])";
    CHECK(build("ceiling_fan", "", "", fan1) == 1);
    const Entity* f = get("zhac_00158d0007a1b2c3_fan");
    CHECK(f && f->component == "fan");
    CHECK(f && std::string(f->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/fan_state/set" && std::string(f->cfg["payload_on"]) == "1");
    CHECK(f && f->cfg["preset_modes"].size() == 4 && std::string(f->cfg["preset_mode_command_topic"]) == "zhac/devices/00158D0007A1B2C3/fan_mode/set");
    CHECK(f && f->cfg["state_value_template"].isNull());
    // the mode word carries the power: "off" and "on"
    const char* fan2 = R"([{"name":"fan_mode","type":"enum","access":3,"values":["off","low","medium","high","on"]}])";
    CHECK(build("fan", "", "", fan2) == 1);
    f = get("zhac_00158d0007a1b2c3_fan");
    CHECK(f && std::string(f->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/fan_mode/set");
    CHECK(f && std::string(f->cfg["payload_on"]) == "on" && std::string(f->cfg["payload_off"]) == "off");
    CHECK(f && std::string(f->cfg["state_value_template"]) == "{{ 'off' if value == 'off' else 'on' }}");
    CHECK(f && f->cfg["preset_modes"].size() == 3 && !has(f->cfg["preset_modes"], "on"));
    // no "off" word and no on/off expose: not a fan, stays a select
    CHECK(build("x", "", "", R"([{"name":"fan_mode","type":"enum","access":3,"values":["low","high"]}])") == 1);
    CHECK(get("zhac_00158d0007a1b2c3_fan_mode") && get("zhac_00158d0007a1b2c3_fan_mode")->component == "select");
    // a binary fan_mode is the switch
    CHECK(build("x", "", "", R"([{"name":"fan_mode","type":"binary","access":3}])") == 1);
    f = get("zhac_00158d0007a1b2c3_fan");
    CHECK(f && std::string(f->cfg["command_topic"]) == "zhac/devices/00158D0007A1B2C3/fan_mode/set");

    char t[96];
    CHECK(ha::device_availability_topic(t, sizeof(t), "zhac", 0xA4C138F3E2D10B77ULL) > 0 &&
          std::string(t) == "zhac/devices/A4C138F3E2D10B77/availability");
}

int main() {
    test_sensors_and_binary();
    test_climate();
    test_cover_lock_fan();
    test_light();
    test_plug_select_number_and_skips();
    test_bad_input_and_bridge();
    test_topics_and_payloads();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("ha_discovery: all checks passed\n");
    return 0;
}
