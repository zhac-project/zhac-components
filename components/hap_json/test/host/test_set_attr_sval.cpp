// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// SET_ATTRIBUTE string values: an enum option travels as "sval" and survives
// the round trip; without one the frame is byte-identical to before, so an
// older P4 sees nothing new.
#include <cstdio>
#include <cstring>

#include "hap_json.h"

int g_esp_loge_count = 0;   // bumped by the stub ESP_LOGE (stubs/esp_log.h)
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    uint8_t buf[256];
    uint16_t len = 0;

    HapSetAttrReq a{};
    a.ieee = 0xA4C138F3E2D10B77ULL;
    std::snprintf(a.key, sizeof(a.key), "power_outage_memory");
    std::snprintf(a.sval, sizeof(a.sval), "restore");
    CHECK(hap_json_encode_set_attr(buf, sizeof(buf), &len, a));
    CHECK(std::strstr(reinterpret_cast<char*>(buf), "\"sval\":\"restore\"") != nullptr);
    HapSetAttrReq b{};
    CHECK(hap_json_decode_set_attr(buf, len, b));
    CHECK(std::strcmp(b.sval, "restore") == 0);
    CHECK(std::strcmp(b.key, "power_outage_memory") == 0);
    CHECK(b.ieee == a.ieee);

    HapSetAttrReq c{};
    c.ieee = 1;
    c.val = 1;
    std::snprintf(c.key, sizeof(c.key), "state");
    CHECK(hap_json_encode_set_attr(buf, sizeof(buf), &len, c));
    buf[len] = 0;
    CHECK(std::strstr(reinterpret_cast<char*>(buf), "sval") == nullptr);   // unchanged wire format
    HapSetAttrReq d{};
    CHECK(hap_json_decode_set_attr(buf, len, d));
    CHECK(d.sval[0] == '\0' && d.val == 1);

    // An over-long option is truncated, never overruns the field.
    const char* longv = "{\"ieee\":\"0x1\",\"key\":\"mode\",\"sval\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
    HapSetAttrReq e{};
    CHECK(hap_json_decode_set_attr(reinterpret_cast<const uint8_t*>(longv), static_cast<uint16_t>(std::strlen(longv)), e));
    CHECK(std::strlen(e.sval) == sizeof(e.sval) - 1);

    // A decimal value rides as "fval" next to the rounded "val"; an integer
    // write carries no fval and decodes with has_fval false.
    HapSetAttrReq f{};
    f.ieee = 2; f.val = 22; f.fval = 21.5f; f.has_fval = true;
    std::snprintf(f.key, sizeof(f.key), "heating_setpoint");
    CHECK(hap_json_encode_set_attr(buf, sizeof(buf), &len, f));
    buf[len] = 0;
    CHECK(std::strstr(reinterpret_cast<char*>(buf), "\"fval\":21.5") != nullptr);
    CHECK(std::strstr(reinterpret_cast<char*>(buf), "\"val\":22") != nullptr);
    HapSetAttrReq g{};
    CHECK(hap_json_decode_set_attr(buf, len, g));
    CHECK(g.has_fval && g.fval > 21.49f && g.fval < 21.51f && g.val == 22);
    CHECK(!d.has_fval && d.fval == 0.0f);   // the integer write above

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hap_json set_attr sval: all checks passed\n");
    return 0;
}
