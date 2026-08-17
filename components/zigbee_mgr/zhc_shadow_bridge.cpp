// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Bridge between zhc_adapter's decode output and device_shadow. Called
// once per emitted key/value from the ZHC library; packs a ZclAttribute
// with the string key + value and hands it to device_shadow_process so
// the existing debounce/throttle/NVS pipeline applies.
//
// String keys are copied verbatim from ZHC's exposes (no translation
// table). The legacy attr_keys integer-ID namespace has been retired.

#include "zhc_adapter.h"
#include "device_shadow.h"
#include "zap_store.h"
#include "zigbee_pool.h"

#include "esp_log.h"
#include <cstdint>
#include <cstring>

static const char* TAG = "zhc_shadow";

namespace {
// zhc::ValueType: 0=None,1=Bool,2=Uint,3=Int,4=Float,5=StringRef
constexpr uint8_t kValueKindBool   = 1;
constexpr uint8_t kValueKindUint   = 2;
constexpr uint8_t kValueKindInt    = 3;
constexpr uint8_t kValueKindFloat  = 4;
constexpr uint8_t kValueKindString = 5;

// Mirror a decoded `battery` percentage into ZapDevice::battery_pct.
//
// That field is read by hap_json (`bat_pct`, the device list the SPA renders)
// and by both firmwares' ws_bridge — and was written by NOTHING, on any core.
// Every device reported 0% battery forever while the real value sat one layer
// away in the shadow as the `battery` key. It is mirrored here rather than in a
// firmware because this is the one place every core's decode output passes
// through.
//
// Scaling matters: a float arrives already multiplied by 100 (see the
// kValueKindFloat case), so a device reporting 87.5% lands as 8750 and would
// otherwise be stored as a nonsense percentage.
//
// Written only on change. Battery moves in single-percent steps over days, but
// a chatty sensor re-reports the SAME value every few seconds — persisting each
// one would turn a cosmetic field into a steady NVS write stream.
void mirror_battery_pct(uint64_t ieee, const ZclAttribute& attr) {
    if (std::strcmp(attr.key, "battery") != 0) return;
    if (attr.val_type != VAL_INT && attr.val_type != VAL_FLOAT) return;

    int32_t pct = attr.int_val;
    if (attr.val_type == VAL_FLOAT) pct /= 100;   // stored as value × 100
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    struct Ctx { uint16_t pct; bool changed; ZapDevice snap; } ctx{
        static_cast<uint16_t>(pct), false, {} };

    zigbee_pool_with_device(ieee, [](ZapDevice* d, void* c) {
        auto* x = static_cast<Ctx*>(c);
        if (d->battery_pct == x->pct) return;     // no change, no write
        d->battery_pct = x->pct;
        x->changed     = true;
        x->snap        = *d;
    }, &ctx);

    // Persist OUTSIDE the visitor: zap_store_mark_dirty can write flash
    // synchronously when the dirty table is full, and that must never run
    // under the pool mutex.
    if (ctx.changed) zap_store_mark_dirty(&ctx.snap, ZAP_PERSIST_LOW);
}

extern "C" void zhc_shadow_update_cb(uint64_t ieee,
                                      const char* key,
                                      uint8_t value_kind,
                                      int64_t int_val,
                                      uint64_t uint_val,
                                      float float_val,
                                      bool bool_val,
                                      const char* str_val) {
    if (!key) return;

    // F6/F35 (FINDINGS.md): pool_find_by_ieee returns a raw pointer whose
    // array slot a concurrent swap-with-last pool_remove can relocate.
    // Snapshot the device under the advisory lock and operate on the copy
    // (device_shadow_process only reads ieee_addr and does not retain it).
    ZapDevice snap;
    bool found = false;
    zigbee_pool_lock();
    if (const ZapDevice* dev = pool_find_by_ieee(ieee)) { snap = *dev; found = true; }
    zigbee_pool_unlock();
    if (!found) {
        ESP_LOGD(TAG, "no device for ieee=0x%016llx",
                 static_cast<unsigned long long>(ieee));
        return;
    }

    ZclAttribute attr{};
    strncpy(attr.key, key, ATTR_KEY_MAX - 1);
    attr.key[ATTR_KEY_MAX - 1] = '\0';
    attr.cluster = 0;
    attr.attr_id = 0;

    switch (value_kind) {
        case kValueKindBool:
            attr.val_type = VAL_BOOL;
            attr.int_val  = bool_val ? 1 : 0;
            break;
        case kValueKindUint:
            attr.val_type = VAL_INT;
            attr.int_val  = static_cast<int32_t>(uint_val);
            break;
        case kValueKindInt:
            attr.val_type = VAL_INT;
            attr.int_val  = static_cast<int32_t>(int_val);
            break;
        case kValueKindFloat:
            // Store value × 100 (2-dp fixed point) tagged VAL_FLOAT: the JSON
            // encoders unscale it (÷100) for display, while rules/Lua compare the
            // raw ×100 integer. Previously tagged VAL_INT, which made a scaled
            // float indistinguishable from a genuine integer — the encoders then
            // guessed by key name (a hardcoded float-key list), so an integer
            // humidity got wrongly ÷100 (→0.49) on the live path while a float
            // temperature showed un-unscaled (×100 → 2900) on the snapshot path.
            attr.val_type = VAL_FLOAT;
            attr.int_val  = static_cast<int32_t>(float_val * 100.0f);
            break;
        case kValueKindString:
            if (!str_val) return;
            attr.val_type = VAL_STR;
            strncpy(attr.str_val, str_val, ATTR_STR_MAX - 1);
            attr.str_val[ATTR_STR_MAX - 1] = '\0';
            break;
        default:
            return;   // None / BytesRef / ObjectRef — not shadowable
    }

    device_shadow_process(&snap, &attr, 1);
    mirror_battery_pct(ieee, attr);
}

}  // namespace

extern "C" void zhc_shadow_bridge_register(void) {
    zhac_adapter_register_shadow(&zhc_shadow_update_cb);
}
