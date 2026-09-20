// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Fakes for the three things device_cmd touches: the device pool, the
// adapter's send functions, the shadow's optimistic update. Each records
// its last call so the test can pin the contract.
#include <cstdio>
#include <cstring>
#include "device_shadow.h"
#include "zap_common.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"
#include "test_stubs.h"

static ZapDevice s_dev{};
static bool s_have_dev = false;
StubSend  g_send{};
StubShadow g_shadow{};
bool g_send_result = true;

void stub_pool_set(uint64_t ieee, uint16_t nwk, uint8_t ep0, const char* model, const char* manu) {
    std::memset(&s_dev, 0, sizeof(s_dev));
    s_dev.ieee_addr = ieee; s_dev.nwk_addr = nwk;
    s_dev.endpoints[0] = ep0; s_dev.endpoint_count = ep0 ? 1 : 0;
    std::snprintf(s_dev.model_id, sizeof(s_dev.model_id), "%s", model);
    std::snprintf(s_dev.manufacturer_name, sizeof(s_dev.manufacturer_name), "%s", manu);
    s_have_dev = true;
}
void stub_pool_clear() { s_have_dev = false; }
void stub_reset() { g_send = StubSend{}; g_shadow = StubShadow{}; g_send_result = true; }

void zigbee_pool_lock()   { g_send.lock_depth++; }
void zigbee_pool_unlock() { g_send.lock_depth--; }
ZapDevice* pool_find_by_ieee(uint64_t ieee) { return (s_have_dev && s_dev.ieee_addr == ieee) ? &s_dev : nullptr; }

static bool rec(const char* kind, uint64_t ieee, const char* model, const char* manu,
                uint16_t nwk, uint8_t ep, const char* key) {
    g_send.calls++;
    g_send.locked_during_send = g_send.lock_depth != 0;   // must be false: the lock is released first
    std::snprintf(g_send.kind, sizeof(g_send.kind), "%s", kind);
    g_send.ieee = ieee; g_send.nwk = nwk; g_send.ep = ep;
    std::snprintf(g_send.model, sizeof(g_send.model), "%s", model);
    std::snprintf(g_send.manu, sizeof(g_send.manu), "%s", manu);
    std::snprintf(g_send.key, sizeof(g_send.key), "%s", key);
    return g_send_result;
}
extern "C" bool zhac_adapter_send_bool(uint64_t ieee, const char* m, const char* n, uint16_t nwk, uint8_t ep, const char* key, bool v) {
    g_send.b = v; return rec("bool", ieee, m, n, nwk, ep, key);
}
extern "C" bool zhac_adapter_send_uint(uint64_t ieee, const char* m, const char* n, uint16_t nwk, uint8_t ep, const char* key, uint64_t v) {
    g_send.u = v; return rec("uint", ieee, m, n, nwk, ep, key);
}
extern "C" bool zhac_adapter_send_float(uint64_t ieee, const char* m, const char* n, uint16_t nwk, uint8_t ep, const char* key, double v) {
    g_send.f = v; return rec("float", ieee, m, n, nwk, ep, key);
}
extern "C" bool zhac_adapter_send_number(uint64_t ieee, const char* m, const char* n, uint16_t nwk, uint8_t ep, const char* key, double v) {
    g_send.f = v; return rec("number", ieee, m, n, nwk, ep, key);
}
extern "C" bool zhac_adapter_send_string(uint64_t ieee, const char* m, const char* n, uint16_t nwk, uint8_t ep, const char* key, const char* v) {
    std::snprintf(g_send.s, sizeof(g_send.s), "%s", v ? v : ""); return rec("string", ieee, m, n, nwk, ep, key);
}
void device_shadow_update_optimistic(uint64_t ieee, const char* key, uint8_t val_type, int32_t val) {
    g_shadow.writes++; g_shadow.ieee = ieee; g_shadow.vt = val_type; g_shadow.val = val;
    std::snprintf(g_shadow.key, sizeof(g_shadow.key), "%s", key);
}
