// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// The attribute-set contract, pinned once for every transport.
#include <cstdio>
#include <cstring>
#include "device_cmd.h"
#include "device_cmd_json.h"
#include "zcl_attribute.h"
#include "test_stubs.h"

static int g_fail = 0;
#define CHECK(c, m) do { if (c) std::printf("PASS: %s\n", m); else { std::printf("FAIL: %s\n", m); g_fail++; } } while (0)
static const uint64_t IEEE = 0x00158D0007A1B2C3ULL;

static void arm() { stub_reset(); stub_pool_set(IEEE, 0x3F1A, 2, "TS0601", "_TZE200_abc"); }

int main() {
    // bool: adapter bool, shadow VAL_BOOL, device default endpoint, lock released before the send
    arm();
    DevCmdValue v = device_cmd_bool(true);
    CHECK(device_cmd_set_attr(IEEE, 0, "state", &v) == DEVCMD_OK, "bool state -> OK");
    CHECK(std::strcmp(g_send.kind, "bool") == 0 && g_send.b && g_send.ep == 2 && g_send.nwk == 0x3F1A &&
          std::strcmp(g_send.model, "TS0601") == 0 && std::strcmp(g_send.manu, "_TZE200_abc") == 0,
          "bool reaches the adapter with the device's identity and default endpoint");
    CHECK(!g_send.locked_during_send, "pool lock is released before the radio send");
    CHECK(g_shadow.writes == 1 && g_shadow.vt == VAL_BOOL && g_shadow.val == 1 && std::strcmp(g_shadow.key, "state") == 0,
          "bool mirrors into the shadow as VAL_BOOL");

    // integer: uint path; "state" as an integer still mirrors as VAL_BOOL
    arm(); v = device_cmd_int(128);
    CHECK(device_cmd_set_attr(IEEE, 0, "brightness", &v) == DEVCMD_OK && std::strcmp(g_send.kind, "uint") == 0 && g_send.u == 128,
          "integer 128 -> adapter uint 128");
    CHECK(g_shadow.writes == 1 && g_shadow.vt == VAL_INT && g_shadow.val == 128, "integer mirrors as VAL_INT");
    arm(); v = device_cmd_int(0);
    device_cmd_set_attr(IEEE, 0, "state", &v);
    CHECK(g_shadow.vt == VAL_BOOL && g_shadow.val == 0, "integer 0 on \"state\" mirrors as VAL_BOOL");

    // negative integer: not a uint; goes as a number (Int), shadow VAL_INT -5
    arm(); v = device_cmd_int(-5);
    CHECK(device_cmd_set_attr(IEEE, 0, "offset", &v) == DEVCMD_OK && std::strcmp(g_send.kind, "number") == 0 && g_send.f == -5.0,
          "negative integer -> adapter number -5");
    CHECK(g_shadow.vt == VAL_INT && g_shadow.val == -5, "negative integer mirrors as VAL_INT -5");

    // decimal: float path, shadow VAL_FLOAT x100
    arm(); v = device_cmd_float(21.5);
    CHECK(device_cmd_set_attr(IEEE, 0, "current_heating_setpoint", &v) == DEVCMD_OK &&
          std::strcmp(g_send.kind, "float") == 0 && g_send.f == 21.5, "decimal 21.5 -> adapter float 21.5");
    CHECK(g_shadow.writes == 1 && g_shadow.vt == VAL_FLOAT && g_shadow.val == 2150, "decimal mirrors as VAL_FLOAT 2150");

    // string: string path, NO shadow write
    arm(); v = device_cmd_str("restore");
    CHECK(device_cmd_set_attr(IEEE, 0, "power_outage_memory", &v) == DEVCMD_OK &&
          std::strcmp(g_send.kind, "string") == 0 && std::strcmp(g_send.s, "restore") == 0, "string -> adapter string");
    CHECK(g_shadow.writes == 0, "string writes no shadow value");

    // explicit endpoint wins over the device default
    arm(); v = device_cmd_bool(false);
    device_cmd_set_attr(IEEE, 3, "state", &v);
    CHECK(g_send.ep == 3, "explicit endpoint 3 reaches the adapter");

    // not found: no adapter call, no shadow
    arm(); stub_pool_clear(); v = device_cmd_bool(true);
    CHECK(device_cmd_set_attr(IEEE, 0, "state", &v) == DEVCMD_NOT_FOUND && g_send.calls == 0 && g_shadow.writes == 0,
          "unknown device -> NOT_FOUND, nothing sent, nothing mirrored");

    // converter refuses: NO_CONVERTER, no shadow (never lie about a command that did not go out)
    arm(); g_send_result = false; v = device_cmd_float(21.5);
    CHECK(device_cmd_set_attr(IEEE, 0, "brightness", &v) == DEVCMD_NO_CONVERTER && g_shadow.writes == 0,
          "refused decimal -> NO_CONVERTER, no shadow write");

    // bad arguments and values
    arm(); v = device_cmd_bool(true);
    CHECK(device_cmd_set_attr(IEEE, 0, nullptr, &v) == DEVCMD_BAD_ARGS && device_cmd_set_attr(IEEE, 0, "", &v) == DEVCMD_BAD_ARGS &&
          device_cmd_set_attr(IEEE, 0, "state", nullptr) == DEVCMD_BAD_ARGS, "missing key or value -> BAD_ARGS");
    v = device_cmd_float(0.0 / 0.0);
    CHECK(device_cmd_set_attr(IEEE, 0, "x", &v) == DEVCMD_BAD_VALUE, "NaN -> BAD_VALUE");
    v = device_cmd_str(nullptr);
    CHECK(device_cmd_set_attr(IEEE, 0, "x", &v) == DEVCMD_BAD_VALUE && g_send.calls == 0, "null string -> BAD_VALUE, nothing sent");

    // JSON values, as the transports receive them
    JsonDocument d;
    deserializeJson(d, R"({"b":true,"i":12,"n":-3,"f":21.5,"w":4.0,"s":"12","e":"restore","z":null,"o":{},"a":[]})");
    DevCmdValue o{};
    CHECK(device_cmd_value_from_json(d["b"], &o) && o.kind == DEVCMD_BOOL && o.b, "JSON true -> BOOL");
    CHECK(device_cmd_value_from_json(d["i"], &o) && o.kind == DEVCMD_INT && o.i == 12, "JSON 12 -> INT 12");
    CHECK(device_cmd_value_from_json(d["n"], &o) && o.kind == DEVCMD_INT && o.i == -3, "JSON -3 -> INT -3");
    CHECK(device_cmd_value_from_json(d["f"], &o) && o.kind == DEVCMD_FLOAT && o.f == 21.5, "JSON 21.5 -> FLOAT");
    CHECK(device_cmd_value_from_json(d["w"], &o) && o.kind == DEVCMD_INT && o.i == 4, "JSON 4.0 -> INT 4 (integral)");
    CHECK(device_cmd_value_from_json(d["s"], &o) && o.kind == DEVCMD_STR && std::strcmp(o.s, "12") == 0, "JSON \"12\" stays a string");
    CHECK(device_cmd_value_from_json(d["e"], &o) && o.kind == DEVCMD_STR, "JSON \"restore\" -> STR");
    CHECK(!device_cmd_value_from_json(d["z"], &o) && !device_cmd_value_from_json(d["o"], &o) && !device_cmd_value_from_json(d["a"], &o),
          "JSON null / object / array -> unsupported");

    // the words every transport answers with
    CHECK(std::strcmp(device_cmd_result_str(DEVCMD_NOT_FOUND), "device not found") == 0 &&
          std::strcmp(device_cmd_result_str(DEVCMD_NO_CONVERTER), "no zhc converter") == 0 &&
          std::strcmp(device_cmd_result_str(DEVCMD_BAD_VALUE), "value must be bool / number / string") == 0,
          "result words are the ones the UI and docs know");

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("device_cmd contract: all checks passed\n");
    return 0;
}
