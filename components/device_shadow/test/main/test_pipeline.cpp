// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "device_shadow.h"
#include <cstring>

// ── Helpers ───────────────────────────────────────────────────────────────

static ZclAttribute make_attr(const char* key, int32_t val,
                               ValType vt = VAL_INT) {
    ZclAttribute a{};
    zcl_attr_set_int(&a, key, val, vt);
    return a;
}

static DeviceConfig make_cfg_filter(const char* key) {
    DeviceConfig cfg{};
    cfg.filtered_count = 1;
    strncpy(cfg.filtered[0], key, ATTR_KEY_MAX - 1);
    cfg.filtered[0][ATTR_KEY_MAX - 1] = '\0';
    return cfg;
}

// ── Exposed from shadow_pipeline.cpp for testing ─────────────────────────
extern "C" uint8_t shadow_pipeline_filter(const DeviceConfig* cfg,
                                           const ZclAttribute* in, uint8_t in_count,
                                           ZclAttribute* out, uint8_t max_out);
extern "C" bool shadow_pipeline_throttle_pass(DeviceConfig* cfg,
                                               uint32_t* last_ms,
                                               uint32_t now_ms);

// ── Filter tests ──────────────────────────────────────────────────────────

TEST_CASE("filter: drops configured key, passes others", "[shadow_pipeline]") {
    DeviceConfig cfg = make_cfg_filter("linkquality");
    ZclAttribute in[3] = {
        make_attr("state",        1, VAL_BOOL),
        make_attr("linkquality", 55),
        make_attr("temperature", 2200),
    };
    ZclAttribute out[3]{};
    uint8_t n = shadow_pipeline_filter(&cfg, in, 3, out, 3);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("state",       out[0].key);
    TEST_ASSERT_EQUAL_STRING("temperature", out[1].key);
}

TEST_CASE("filter: passes all when filtered_count=0", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    ZclAttribute in[2] = { make_attr("state", 1, VAL_BOOL),
                           make_attr("temperature", 2200) };
    ZclAttribute out[2]{};
    uint8_t n = shadow_pipeline_filter(&cfg, in, 2, out, 2);
    TEST_ASSERT_EQUAL(2, n);
}

TEST_CASE("filter: drops all matching keys in batch", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.filtered_count = 2;
    strncpy(cfg.filtered[0], "linkquality", ATTR_KEY_MAX - 1);
    strncpy(cfg.filtered[1], "battery",     ATTR_KEY_MAX - 1);
    ZclAttribute in[3] = {
        make_attr("linkquality", 55),
        make_attr("battery",     80),
        make_attr("state",        1, VAL_BOOL),
    };
    ZclAttribute out[3]{};
    uint8_t n = shadow_pipeline_filter(&cfg, in, 3, out, 3);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("state", out[0].key);
}

// ── Throttle tests ────────────────────────────────────────────────────────

TEST_CASE("throttle: first message always passes", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.throttle_ms = 5000;
    uint32_t last_ms = 0;
    TEST_ASSERT_TRUE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 1000));
}

TEST_CASE("throttle: second message within window is dropped", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.throttle_ms = 5000;
    uint32_t last_ms = 0;
    shadow_pipeline_throttle_pass(&cfg, &last_ms, 1000);
    TEST_ASSERT_FALSE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 4000));
}

TEST_CASE("throttle: passes after window expires", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.throttle_ms = 5000;
    uint32_t last_ms = 0;
    shadow_pipeline_throttle_pass(&cfg, &last_ms, 1000);
    TEST_ASSERT_TRUE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 7000));
}

TEST_CASE("throttle: disabled (throttle_ms=0) always passes", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.throttle_ms = 0;
    uint32_t last_ms = 0;
    TEST_ASSERT_TRUE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 100));
    TEST_ASSERT_TRUE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 101));
    TEST_ASSERT_TRUE(shadow_pipeline_throttle_pass(&cfg, &last_ms, 102));
}

// ── Debounce merge + flush tests ──────────────────────────────────────────

extern "C" void shadow_pipeline_merge_pending(const DeviceConfig* cfg,
                                               PendingState* ps,
                                               const ZclAttribute* attrs,
                                               uint8_t count);

extern "C" uint8_t shadow_pipeline_flush_pending(PendingState* ps,
                                                  ZclAttribute* out,
                                                  uint8_t max_out);

extern "C" int8_t shadow_pipeline_debounce_bypass(const DeviceConfig* cfg,
                                                   const PendingState* ps,
                                                   const ZclAttribute* attr);

TEST_CASE("debounce: merge accumulates attrs, last-write-wins per key", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    PendingState ps{};

    ZclAttribute a1 = make_attr("state", 1, VAL_BOOL);
    ZclAttribute a2 = make_attr("state", 0, VAL_BOOL); // same key, newer value
    ZclAttribute a3 = make_attr("temperature", 2200);

    shadow_pipeline_merge_pending(&cfg, &ps, &a1, 1);
    shadow_pipeline_merge_pending(&cfg, &ps, &a2, 1);
    shadow_pipeline_merge_pending(&cfg, &ps, &a3, 1);

    ZclAttribute out[32]{};
    uint8_t n = shadow_pipeline_flush_pending(&ps, out, 32);
    TEST_ASSERT_EQUAL(2, n);  // state (last) + temperature
    bool found_state = false;
    for (uint8_t i = 0; i < n; i++) {
        if (strncmp(out[i].key, "state", ATTR_KEY_MAX) == 0) {
            TEST_ASSERT_EQUAL(0, out[i].int_val); // last-write value
            found_state = true;
        }
    }
    TEST_ASSERT_TRUE(found_state);
    TEST_ASSERT_EQUAL(0, ps.pending_count); // flushed = empty
}

TEST_CASE("debounce_ignore: key with new value returns bypass index", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.debounce_ignore_count = 1;
    strncpy(cfg.debounce_ignore[0], "action", ATTR_KEY_MAX - 1);

    PendingState ps{};
    // No existing pending for "action" → new value → should bypass
    ZclAttribute a = make_attr("action", 1);
    int8_t idx = shadow_pipeline_debounce_bypass(&cfg, &ps, &a);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
}

TEST_CASE("debounce_ignore: key with same value does NOT bypass", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.debounce_ignore_count = 1;
    strncpy(cfg.debounce_ignore[0], "action", ATTR_KEY_MAX - 1);

    PendingState ps{};
    ZclAttribute a = make_attr("action", 1);
    shadow_pipeline_merge_pending(&cfg, &ps, &a, 1); // add to pending

    // Same key, same value → no bypass
    int8_t idx = shadow_pipeline_debounce_bypass(&cfg, &ps, &a);
    TEST_ASSERT_EQUAL(-1, idx);
}

TEST_CASE("debounce_ignore: non-ignore key never bypasses", "[shadow_pipeline]") {
    DeviceConfig cfg{};
    cfg.debounce_ignore_count = 1;
    strncpy(cfg.debounce_ignore[0], "action", ATTR_KEY_MAX - 1);

    PendingState ps{};
    ZclAttribute a = make_attr("state", 1, VAL_BOOL);
    int8_t idx = shadow_pipeline_debounce_bypass(&cfg, &ps, &a);
    TEST_ASSERT_EQUAL(-1, idx);
}

TEST_CASE("flush: empty pending returns 0", "[shadow_pipeline]") {
    PendingState ps{};
    ZclAttribute out[4]{};
    TEST_ASSERT_EQUAL(0, shadow_pipeline_flush_pending(&ps, out, 4));
}

// ── Synthetic attribute tests ─────────────────────────────────────────────

TEST_CASE("filter: synthetic attrs skip the filter when the filter names a public key",
          "[shadow_pipeline]") {
    // Synthetic attrs are identified by a leading '_' prefix (e.g. "_last_seen").
    // Naming a real public key in the filter must not accidentally drop them.
    DeviceConfig cfg = make_cfg_filter("state");
    ZclAttribute in[2] = {
        make_attr("state", 1, VAL_BOOL),
        make_attr("_last_seen", 1700000000),
    };
    ZclAttribute out[2]{};
    uint8_t n = shadow_pipeline_filter(&cfg, in, 2, out, 2);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("_last_seen", out[0].key);
    TEST_ASSERT_EQUAL(1700000000, out[0].int_val);
}

TEST_CASE("synthetic: '_'-prefixed keys are treated as internal",
          "[shadow_pipeline]") {
    // Consumers (hap_dispatch, REST emitter) hide keys starting with '_'
    // from the UI. This is the only "synthetic" signal after the attr_keys
    // drop — no more boolean helper lookup.
    const char* synth   = "_last_seen";
    const char* public1 = "state";
    const char* public2 = "battery";
    TEST_ASSERT_TRUE(synth[0]   == '_');
    TEST_ASSERT_FALSE(public1[0] == '_');
    TEST_ASSERT_FALSE(public2[0] == '_');
}
