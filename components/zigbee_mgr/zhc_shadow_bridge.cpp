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
            // Shadow stores int32 only; scale by 100 to preserve 2 dp.
            // Consumers that need exact floats go through the event bus
            // (not shadow). Matches z2m's .withValueStep(0.01) pattern.
            attr.val_type = VAL_INT;
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
}

}  // namespace

extern "C" void zhc_shadow_bridge_register(void) {
    zhac_adapter_register_shadow(&zhc_shadow_update_cb);
}
