// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression: a rule `zigbee.set` action optimistically updates the
// device shadow after a successful send — mirroring the webui SET_ATTRIBUTE
// path (hap_dispatch.cpp). Many devices (esp. Tuya LED drivers) emit no
// attribute report after a command-driven change, so without this a
// rule-issued on/off never reflects in the shadow: the SPA reverts to the
// last-known value and the rule looks like it did nothing (even though the
// bulb obeyed the identical ZCL frame the webui path sends). Guarded on send
// success so a failed command writes no (lying) shadow value.
#include "simple_rules.h"
#include "event_bus.h"
#include "zigbee_pool.h"
#include "zcl_attribute.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

// Provided by stubs/host_stubs.cpp.
extern void        stub_pool_seed(const ZapDevice* dev);
extern void        stub_shadow_opt_reset(void);
extern int         stub_shadow_opt_count(void);
extern uint64_t    stub_shadow_opt_ieee(void);
extern const char* stub_shadow_opt_key(void);
extern uint8_t     stub_shadow_opt_vt(void);
extern int32_t     stub_shadow_opt_val(void);
extern void        stub_adapter_send_set_result(bool r);

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

static constexpr uint64_t kIeee = 0xa4c138d5f501d501ULL;

// Publish one decoded attribute as a ZCL_ATTR event (drives the matcher).
static void publish_attr(const char* key, uint8_t vt, int32_t v) {
    Event ev{};
    ev.type = EventType::ZCL_ATTR;
    auto& ze = *reinterpret_cast<ZclAttrEvent*>(ev.data);
    ze.ieee = kIeee;
    ze.val_type = vt;
    std::strncpy(ze.key, key, ATTR_KEY_MAX - 1);
    ze.key[ATTR_KEY_MAX - 1] = '\0';
    ze.int_val = v;
    event_bus_publish(ev);
}

static void drain() {
    for (int i = 0; i < 8; i++)
        if (event_bus_drain(EventType::ZCL_ATTR, 0) == 0) break;
}

int main() {
    event_bus_init();
    simple_rules_init();

    // One device named "lamp" so `zigbee.set "lamp" ...` resolves its target.
    ZapDevice dev{};
    dev.ieee_addr      = kIeee;
    dev.nwk_addr       = 0x1274;
    dev.endpoints[0]   = 1;
    dev.endpoint_count = 1;
    std::strncpy(dev.friendly_name, "lamp", sizeof(dev.friendly_name) - 1);
    std::strncpy(dev.model_id, "TS0503B", sizeof(dev.model_id) - 1);
    stub_pool_seed(&dev);

    uint16_t id = 0;

    // ── state=1 → optimistic (ieee, "state", VAL_BOOL, 1) ────────────────
    stub_shadow_opt_reset();
    CHECK(simple_rules_add("r_on",
            "ON motion#occupancy=1 DO zigbee.set lamp state 1 ENDON", &id),
          "install state-on rule");
    publish_attr("occupancy", VAL_BOOL, 1);
    drain();
    CHECK(stub_shadow_opt_count() == 1,
          "zigbee.set state 1 writes the shadow exactly once");
    CHECK(stub_shadow_opt_ieee() == kIeee &&
          strcmp(stub_shadow_opt_key(), "state") == 0 &&
          stub_shadow_opt_vt() == VAL_BOOL &&
          stub_shadow_opt_val() == 1,
          "optimistic write = (ieee, state, VAL_BOOL, 1)");

    // ── state=0 → VAL_BOOL 0 ─────────────────────────────────────────────
    stub_shadow_opt_reset();
    CHECK(simple_rules_add("r_off",
            "ON contact_dev#contact=1 DO zigbee.set lamp state 0 ENDON", &id),
          "install state-off rule");
    publish_attr("contact", VAL_BOOL, 1);
    drain();
    CHECK(stub_shadow_opt_count() == 1 &&
          strcmp(stub_shadow_opt_key(), "state") == 0 &&
          stub_shadow_opt_vt() == VAL_BOOL &&
          stub_shadow_opt_val() == 0,
          "zigbee.set state 0 writes (state, VAL_BOOL, 0)");

    // ── a non-state key → VAL_INT ────────────────────────────────────────
    stub_shadow_opt_reset();
    CHECK(simple_rules_add("r_dim",
            "ON lux_dev#illuminance=1 DO zigbee.set lamp brightness 128 ENDON", &id),
          "install brightness rule");
    publish_attr("illuminance", VAL_INT, 1);
    drain();
    CHECK(stub_shadow_opt_count() == 1 &&
          strcmp(stub_shadow_opt_key(), "brightness") == 0 &&
          stub_shadow_opt_vt() == VAL_INT &&
          stub_shadow_opt_val() == 128,
          "zigbee.set brightness 128 writes (brightness, VAL_INT, 128)");

    // ── failed send → NO shadow write (never lie about a command that
    //    didn't go out) ───────────────────────────────────────────────────
    stub_shadow_opt_reset();
    stub_adapter_send_set_result(false);
    CHECK(simple_rules_add("r_fail",
            "ON smoke_dev#smoke=1 DO zigbee.set lamp state 1 ENDON", &id),
          "install fail-path rule");
    publish_attr("smoke", VAL_BOOL, 1);
    drain();
    CHECK(stub_shadow_opt_count() == 0,
          "failed zigbee.set writes NO optimistic shadow value");
    stub_adapter_send_set_result(true);

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
