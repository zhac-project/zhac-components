// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
// One decoded Zigbee attribute — string-keyed. Replaces the integer-ID
// `attr_key_id_t` layout from the retired `components/attr_keys/` table.
// Lives in zap_common so every consumer (device_shadow, event_bus,
// zigbee_mgr, simple_rules, lua_engine, hap_dispatch) shares the same
// struct without depending on any legacy component.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Value type tag. Replaces the old `attr_val_type_t` enum.
enum ValType : uint8_t {
    VAL_NONE = 0,
    VAL_INT  = 1,  // int_val interpreted as int32_t
    VAL_BOOL = 2,  // int_val interpreted as 0/1
    VAL_STR  = 3,  // str_val holds a null-terminated string
};

// Key / value buffer sizes. Schema v6 widened both:
//   ATTR_KEY_MAX 20→28 covers `color_temperature_startup` (25 chars +
//   NUL), `color_temperature_min/max`, `hue_powerup_behavior`,
//   `power_outage_count`. v5's 20-cap silently truncated these and
//   made distinct exposes share a 19-char prefix in the shadow cache
//   (SHA-F1 in docs/FINDINGS.md).
//   ATTR_STR_MAX 32→48 covers compound action labels emitted by Tuya /
//   Aqara devices, e.g. `brightness_step_up_color_temperature_step_up`
//   (43 chars + NUL), `hue_and_saturation_move_stop` (28), various
//   `single_long_press_release` permutations (SHA-F8).
// NVS_SHADOW_VERSION must stay in lockstep with both constants (see
// device_shadow.cpp). ZclAttrEvent's _pad shrinks to 4 to keep the
// 96-byte event-bus payload contract intact.
static constexpr uint8_t ATTR_KEY_MAX = 28;
static constexpr uint8_t ATTR_STR_MAX = 48;

struct __attribute__((packed)) ZclAttribute {
    char     key[ATTR_KEY_MAX];   // 0-27   null-terminated
    uint8_t  val_type;            // 28     ValType
    uint8_t  _pad;                // 29
    uint16_t cluster;             // 30-31  origin cluster (0 if synthetic)
    uint16_t attr_id;             // 32-33  origin attr id (0 if synthetic)
    uint16_t _pad2;               // 34-35
    union {
        int32_t int_val;                // 36-39 when val_type==VAL_INT/BOOL
        char    str_val[ATTR_STR_MAX];  // 36-83 when val_type==VAL_STR
    };
};
static_assert(sizeof(ZclAttribute) == 84);
// Defense in depth — packed-struct rearrangements that preserve sizeof
// would silently shift the union and corrupt every NVS-loaded blob
// (SHA-F11). These fail the build the moment the layout drifts.
static_assert(offsetof(ZclAttribute, val_type) == ATTR_KEY_MAX);
static_assert(offsetof(ZclAttribute, int_val)  == 36);

// Helper: populate an INT/BOOL attribute by name. The optional
// `ZCL_ATTR_ASSERT_KEY_FITS` macro turns silent key truncation into a
// hard abort — useful in host tests; firmware leaves it off so a
// runaway vendor port cannot brick the device.
inline void zcl_attr_set_int(ZclAttribute* a, const char* key,
                              int32_t val, ValType t = VAL_INT) {
    // strncpy(dst, NULL, n) is UB; the assert macro only guarded against
    // an over-long key, not a null one (FINDINGS §8). Substitute "" so a
    // null key yields a well-defined empty-keyed attribute instead of a
    // crash on the decode path.
    if (!key) key = "";
#ifdef ZCL_ATTR_ASSERT_KEY_FITS
    if (std::strlen(key) >= ATTR_KEY_MAX) std::abort();
#endif
    std::strncpy(a->key, key, ATTR_KEY_MAX - 1);
    a->key[ATTR_KEY_MAX - 1] = '\0';
    a->val_type = static_cast<uint8_t>(t);
    a->int_val  = val;
    a->cluster  = 0;
    a->attr_id  = 0;
}

// Helper: populate a STR attribute by name.
inline void zcl_attr_set_str(ZclAttribute* a, const char* key,
                              const char* val) {
    // Guard BOTH operands: `val` was already null-checked at the copy
    // site, but `key` reached strncpy raw — a null key is UB (FINDINGS
    // §8). Substitute "" for either so the decode path can't crash.
    if (!key) key = "";
#ifdef ZCL_ATTR_ASSERT_KEY_FITS
    if (std::strlen(key) >= ATTR_KEY_MAX) std::abort();
#endif
    std::strncpy(a->key, key, ATTR_KEY_MAX - 1);
    a->key[ATTR_KEY_MAX - 1] = '\0';
    a->val_type = VAL_STR;
    std::strncpy(a->str_val, val ? val : "", ATTR_STR_MAX - 1);
    a->str_val[ATTR_STR_MAX - 1] = '\0';
    a->cluster  = 0;
    a->attr_id  = 0;
}
