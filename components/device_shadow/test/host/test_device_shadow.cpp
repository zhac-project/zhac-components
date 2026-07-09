// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Characterization harness for the device_shadow component's PUBLIC API,
// exercised end-to-end against the REAL device_shadow.cpp + shadow_pipeline.cpp
// linked over the proven zap_store host shims (in-memory NVS + esp/FreeRTOS
// no-ops) and the REAL event_bus + zap_store (both host-buildable). Same shape
// as components/zap_store/test/host and components/rule_store/test/host.
//
// What is covered (all deterministic, single-threaded):
//   • update_optimistic → get_attr/get_attrs round-trip (value + type)
//   • multiple keys / multiple devices (isolation by IEEE) / in-place overwrite
//   • set/get_config round-trip; set_occupancy_timeout / _debounce_ms /
//     _throttle_ms reflected in get_config; find-only vs find-or-create nuance
//   • clear_attrs (empties attrs, keeps slot+config) vs remove (drops the slot)
//   • device_shadow_process: pipeline pass-through upsert (INT/BOOL/STR), the
//     filtered-key drop, _last_seen injection, occupancy-timer arm, and the
//     debounce buffer→flush-on-disable path
//   • the pure pipeline helpers (filter / throttle / debounce-bypass / merge /
//     flush) driven directly with explicit timestamps
//   • persistence: set_config writes a CRC-headed 'c' blob synchronously;
//     restore_from_pool rehydrates config + attrs from seeded NVS blobs; the
//     F26/T27 CRC guards reject a corrupt attr / config blob on load
//
// NOT host-testable (documented, timer/task driven — the FreeRTOS timer service
// task and task_shadow sweep do not exist on the host):
//   • debounce/occupancy TIMER FIRING (synthetic occupancy=0 on TTL expiry,
//     the sweep's deferred attr-blob NVS write). We characterize the arm/disarm
//     bookkeeping and the flush-on-config-change path instead.
//   • the restore→_last_seen→zap_store_mark_dirty branch (firmware wiring;
//     device_shadow only links zap_store to resolve the symbol).
//
// Characterization only — asserts the component's ACTUAL behavior; it never
// modifies device_shadow. A failing CHECK means a prediction was wrong (fix the
// test) or the contract genuinely drifted (investigate).
#include "device_shadow.h"
#include "event_bus.h"
#include "zap_store.h"
#include "zap_common.h"
#include "zcl_attribute.h"
#include "nvs.h"            // seed / read NVS blobs directly (in-memory stub)
#include "esp_rom_crc.h"    // build valid on-disk CRCs (same fn device_shadow uses)

#include <cstdio>
#include <cstring>
#include <cstdint>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── The pipeline helpers are extern "C" in device_shadow.cpp; call directly ──
extern "C" uint8_t shadow_pipeline_filter(const DeviceConfig*, const ZclAttribute*, uint8_t,
                                          ZclAttribute*, uint8_t);
extern "C" bool    shadow_pipeline_throttle_pass(DeviceConfig*, uint32_t*, uint32_t);
extern "C" int8_t  shadow_pipeline_debounce_bypass(const DeviceConfig*, const PendingState*,
                                                   const ZclAttribute*);
extern "C" void    shadow_pipeline_merge_pending(const DeviceConfig*, PendingState*,
                                                 const ZclAttribute*, uint8_t);
extern "C" uint8_t shadow_pipeline_flush_pending(PendingState*, ZclAttribute*, uint8_t);

// NVS stub reset (defined in stubs/nvs_stub.cpp).
extern "C" void nvs_stub_reset(void);

// ── Mirror of device_shadow.cpp internals needed to seed/inspect NVS ────────
// shadow_key() (base36, 15-char cap) — copied verbatim so seeded blobs land on
// the exact keys restore_from_pool reads.
static void shadow_key(char out[16], char prefix, uint64_t ieee) {
    out[0] = prefix;
    char tmp[16];
    int n = 0;
    do {
        unsigned d = (unsigned)(ieee % 36u);
        ieee /= 36u;
        tmp[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
    } while (ieee);
    int w = 1;
    while (n > 0) out[w++] = tmp[--n];
    out[w] = '\0';
}

// On-disk blob headers (F26 attr / T27 config). Kept in lockstep with
// device_shadow.cpp — a layout drift here should break these tests loudly.
struct __attribute__((packed)) TestShadowBlobHdr { uint8_t ver; uint8_t count; uint16_t _pad; uint32_t crc; };
struct __attribute__((packed)) TestCfgBlobHdr    { uint8_t ver; uint8_t _r0;  uint16_t _pad; uint32_t crc; };
static_assert(sizeof(TestShadowBlobHdr) == 8, "attr blob header must be 8 bytes");
static_assert(sizeof(TestCfgBlobHdr) == 8, "config blob header must be 8 bytes");
static constexpr uint8_t kAttrBlobVer = 1;
static constexpr uint8_t kCfgBlobVer  = 1;

static void nvs_put(char prefix, uint64_t ieee, const uint8_t* buf, size_t len) {
    nvs_handle_t h;
    nvs_open("zap_shadow", NVS_READWRITE, &h);   // stub is namespace-agnostic
    char key[16];
    shadow_key(key, prefix, ieee);
    nvs_set_blob(h, key, buf, len);
    nvs_commit(h);
    nvs_close(h);
}

// Seed a valid (or, with corrupt=true, CRC-broken) attr blob for `ieee`.
static void seed_attr_blob(uint64_t ieee, const ShadowAttr* attrs, uint8_t count, bool corrupt) {
    uint8_t buf[sizeof(TestShadowBlobHdr) + 32 * sizeof(ShadowAttr)];
    size_t plen = (size_t)count * sizeof(ShadowAttr);
    memcpy(buf + sizeof(TestShadowBlobHdr), attrs, plen);
    auto* h  = reinterpret_cast<TestShadowBlobHdr*>(buf);
    h->ver   = kAttrBlobVer;
    h->count = count;
    h->_pad  = 0;
    h->crc   = esp_rom_crc32_le(0, buf + sizeof(TestShadowBlobHdr), (uint32_t)plen);
    if (corrupt) h->crc ^= 0xFFFFFFFFu;
    nvs_put('a', ieee, buf, sizeof(TestShadowBlobHdr) + plen);
}

static void seed_cfg_blob(uint64_t ieee, const DeviceConfig& cfg, bool corrupt) {
    uint8_t buf[sizeof(TestCfgBlobHdr) + sizeof(DeviceConfig)];
    memcpy(buf + sizeof(TestCfgBlobHdr), &cfg, sizeof(DeviceConfig));
    auto* h = reinterpret_cast<TestCfgBlobHdr*>(buf);
    h->ver  = kCfgBlobVer;
    h->_r0  = 0;
    h->_pad = 0;
    h->crc  = esp_rom_crc32_le(0, buf + sizeof(TestCfgBlobHdr), (uint32_t)sizeof(DeviceConfig));
    if (corrupt) h->crc ^= 0xFFFFFFFFu;
    nvs_put('c', ieee, buf, sizeof(buf));
}

// Read back a config blob device_shadow wrote (to prove synchronous persist).
static bool read_cfg_blob(uint64_t ieee, DeviceConfig* out) {
    nvs_handle_t h;
    nvs_open("zap_shadow", NVS_READWRITE, &h);
    char key[16];
    shadow_key(key, 'c', ieee);
    uint8_t buf[sizeof(TestCfgBlobHdr) + sizeof(DeviceConfig)];
    size_t len = sizeof(buf);
    esp_err_t e = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(buf)) return false;
    memcpy(out, buf + sizeof(TestCfgBlobHdr), sizeof(DeviceConfig));
    return true;
}

// ── Small helpers ───────────────────────────────────────────────────────────
static bool keyeq(const char* a, const char* b) { return strncmp(a, b, ATTR_KEY_MAX) == 0; }

// update_optimistic uses find_entry (NOT find_or_create): the entry must exist
// first. A device becomes known via set_config (mirrors the SPA configuring a
// device before optimistic command writes land). Zeroed config = no _last_seen
// injection, no debounce/throttle — so attr_count reflects only what we write.
static void make_known(uint64_t ieee) {
    DeviceConfig c{};
    c.last_seen_enabled = false;
    device_shadow_set_config(ieee, &c);
}

static ShadowAttr make_attr(const char* key, uint8_t val_type, int32_t int_val, uint32_t ts) {
    ShadowAttr a{};
    strncpy(a.key, key, ATTR_KEY_MAX - 1);
    a.key[ATTR_KEY_MAX - 1] = '\0';
    a.val_type = val_type;
    a.int_val  = int_val;
    a.ts       = ts;
    return a;
}

int main() {
    printf("test_device_shadow (public API + pipeline over in-memory NVS)\n");

    // Lifecycle: init once. device_shadow_init allocates the table, opens the
    // shadow NVS namespace and stamps the schema version; the housekeeping task
    // it spawns is a no-op on the host (see stubs/freertos/task.h).
    device_shadow_init();
    event_bus_init();   // publishes with zero subscribers are safe no-ops
    zap_store_init();   // linked to resolve zap_store_mark_dirty (restore path)

    // ── G1: update_optimistic → get_attr/get_attrs round-trip ──────────────
    {
        printf("\nG1 optimistic writes + reads\n");
        const uint64_t A = 0x00A1ULL;
        make_known(A);

        ShadowAttr o{};
        CHECK(!device_shadow_get_attr(A, "state", &o), "no attr before first write");

        device_shadow_update_optimistic(A, "state", VAL_BOOL, 1);
        CHECK(device_shadow_get_attr(A, "state", &o), "get_attr finds written key");
        CHECK(o.val_type == VAL_BOOL && o.int_val == 1, "state round-trips value+type (BOOL 1)");

        device_shadow_update_optimistic(A, "level", VAL_INT, 200);
        CHECK(device_shadow_get_attr(A, "level", &o) && o.int_val == 200 && o.val_type == VAL_INT,
              "second key round-trips (INT 200)");

        device_shadow_update_optimistic(A, "temperature", VAL_FLOAT, 2350);
        CHECK(device_shadow_get_attr(A, "temperature", &o) && o.int_val == 2350 && o.val_type == VAL_FLOAT,
              "FLOAT attr keeps raw x100 int + VAL_FLOAT tag");

        ShadowAttr all[8]{};
        uint8_t n = device_shadow_get_attrs(A, all, 8);
        CHECK(n == 3, "get_attrs returns all three attrs for the device");

        CHECK(device_shadow_get_attrs(A, all, 1) == 1, "get_attrs honours max_count clamp");
    }

    // ── G2: in-place overwrite (no slot growth) ────────────────────────────
    {
        printf("\nG2 overwrite in place\n");
        const uint64_t A = 0x00A1ULL;   // same device from G1
        device_shadow_update_optimistic(A, "level", VAL_INT, 50);
        ShadowAttr o{};
        CHECK(device_shadow_get_attr(A, "level", &o) && o.int_val == 50,
              "re-writing an existing key updates the value");
        ShadowAttr all[8]{};
        CHECK(device_shadow_get_attrs(A, all, 8) == 3, "overwrite did NOT add a new slot (still 3)");
    }

    // ── G2b: optimistic write EMITS SHADOW_OPTIMISTIC (the HAP → cloud/local
    //    push) but NOT ZCL_ATTR (which the rule engine subscribes to). Without
    //    this event a no-report device (Tuya LED) never reflects a command-
    //    driven change past the P4 cache — the cloud webUI stays stale.
    //    This harness's queue stub is a no-op (event *delivery* is covered in
    //    the event_bus suite), so probe the publish synchronously via a
    //    subscriber FILTER — it runs in the publisher's task, before the
    //    (stubbed) enqueue, so it observes the emit without a live queue.
    {
        printf("\nG2b optimistic emit: SHADOW_OPTIMISTIC yes, ZCL_ATTR no\n");
        const uint64_t A = 0x00A1ULL;   // known device from G1

        ZclAttrEvent got{};
        int opt_seen = 0, zcl_seen = 0;
        auto noop = [](const Event&){};
        EventSubHandle opt = event_bus_subscribe(EventType::SHADOW_OPTIMISTIC, noop,
            [&](const Event& e){ opt_seen++; got = *reinterpret_cast<const ZclAttrEvent*>(e.data); return false; });
        EventSubHandle zcl = event_bus_subscribe(EventType::ZCL_ATTR, noop,
            [&](const Event&){ zcl_seen++; return false; });

        device_shadow_update_optimistic(A, "state", VAL_BOOL, 1);
        CHECK(opt_seen == 1, "optimistic write publishes exactly one SHADOW_OPTIMISTIC event");
        CHECK(got.ieee == A && strcmp(got.key, "state") == 0 &&
              got.val_type == VAL_BOOL && got.int_val == 1,
              "SHADOW_OPTIMISTIC payload carries ieee/key/type/value");
        CHECK(zcl_seen == 0, "optimistic write emits NO ZCL_ATTR (rule engine won't self-trigger)");

        device_shadow_update_optimistic(0x0DEADULL, "state", VAL_BOOL, 1);  // unknown → find-only
        CHECK(opt_seen == 1, "unknown device: no optimistic emit");

        event_bus_unsubscribe(opt);   // filters capture block-locals — remove before they dangle
        event_bus_unsubscribe(zcl);
    }

    // ── G3: isolation by IEEE + unknown-device reads ───────────────────────
    {
        printf("\nG3 multi-device isolation\n");
        const uint64_t A = 0x00A1ULL;
        const uint64_t B = 0x00B2ULL;
        make_known(B);
        device_shadow_update_optimistic(B, "humidity", VAL_FLOAT, 5000);

        ShadowAttr o{};
        ShadowAttr all[8]{};
        CHECK(device_shadow_get_attr(B, "humidity", &o) && o.int_val == 5000, "B has its own attr");
        CHECK(!device_shadow_get_attr(A, "humidity", &o), "A does NOT see B's attr");
        CHECK(!device_shadow_get_attr(B, "level", &o), "B does NOT see A's attr");
        CHECK(device_shadow_get_attrs(A, all, 0) == 0, "get_attrs with max_count=0 returns 0");

        CHECK(device_shadow_get_attrs(A, all, 8) == 3, "A still has exactly its 3 attrs");
        CHECK(device_shadow_get_attrs(B, all, 8) == 1, "B has exactly its 1 attr");

        const uint64_t U = 0x0DEADULL;
        CHECK(device_shadow_get_attrs(U, all, 8) == 0, "unknown IEEE get_attrs returns 0");
        CHECK(!device_shadow_get_attr(U, "state", &all[0]), "unknown IEEE get_attr returns false");
        device_shadow_update_optimistic(U, "state", VAL_INT, 1);   // no entry → no-op
        CHECK(device_shadow_get_attrs(U, all, 8) == 0,
              "update_optimistic on an unknown IEEE creates nothing (find-only)");
    }

    // ── G4: config CRUD + setters + find-only vs find-or-create ─────────────
    {
        printf("\nG4 config CRUD + setters\n");
        const uint64_t C = 0x00C3ULL;
        DeviceConfig cfg{};
        cfg.debounce_ms         = 500;
        cfg.throttle_ms         = 250;
        cfg.last_seen_enabled   = true;
        cfg.optimistic          = true;
        cfg.occupancy_timeout_s = 60;
        cfg.filtered_count      = 1;
        strncpy(cfg.filtered[0], "linkquality", ATTR_KEY_MAX - 1);
        cfg.debounce_ignore_count = 1;
        strncpy(cfg.debounce_ignore[0], "action", ATTR_KEY_MAX - 1);

        CHECK(device_shadow_set_config(C, &cfg), "set_config on a new IEEE returns true (creates entry)");

        DeviceConfig got{};
        CHECK(device_shadow_get_config(C, &got), "get_config returns true for a known device");
        CHECK(got.debounce_ms == 500 && got.throttle_ms == 250 && got.occupancy_timeout_s == 60,
              "config scalars round-trip");
        CHECK(got.last_seen_enabled && got.optimistic, "config bools round-trip");
        CHECK(got.filtered_count == 1 && keyeq(got.filtered[0], "linkquality"),
              "filtered list round-trips");
        CHECK(got.debounce_ignore_count == 1 && keyeq(got.debounce_ignore[0], "action"),
              "debounce_ignore list round-trips");

        DeviceConfig none{};
        CHECK(!device_shadow_get_config(0x0DEADULL, &none), "get_config on unknown IEEE returns false");

        // Setters mutate + persist; verify via get_config.
        CHECK(device_shadow_set_occupancy_timeout(C, 120), "set_occupancy_timeout on known device true");
        CHECK(device_shadow_get_config(C, &got) && got.occupancy_timeout_s == 120,
              "occupancy timeout updated");

        CHECK(device_shadow_set_throttle_ms(C, 999), "set_throttle_ms true");
        CHECK(device_shadow_get_config(C, &got) && got.throttle_ms == 999, "throttle updated");

        CHECK(device_shadow_set_debounce_ms(C, 0), "set_debounce_ms true");
        CHECK(device_shadow_get_config(C, &got) && got.debounce_ms == 0, "debounce updated to 0");

        // find-only vs find-or-create nuance:
        CHECK(!device_shadow_set_occupancy_timeout(0xBEEFULL, 30),
              "set_occupancy_timeout on unknown IEEE returns false (find-only)");
        CHECK(device_shadow_set_debounce_ms(0xBEE5ULL, 0),
              "set_debounce_ms on unknown IEEE returns true (find-or-create)");
        CHECK(device_shadow_get_config(0xBEE5ULL, &got),
              "set_debounce_ms created the entry");
        CHECK(device_shadow_set_throttle_ms(0xBEE6ULL, 100),
              "set_throttle_ms on unknown IEEE returns true (find-or-create)");
    }

    // ── G5: clear_attrs vs remove ──────────────────────────────────────────
    {
        printf("\nG5 clear_attrs vs remove\n");
        const uint64_t D = 0x00D4ULL;
        DeviceConfig dcfg{};
        dcfg.occupancy_timeout_s = 42;
        device_shadow_set_config(D, &dcfg);
        device_shadow_update_optimistic(D, "state", VAL_BOOL, 1);
        ShadowAttr all[8]{};
        CHECK(device_shadow_get_attrs(D, all, 8) == 1, "device has one attr before clear");

        device_shadow_clear_attrs(D);
        CHECK(device_shadow_get_attrs(D, all, 8) == 0, "clear_attrs empties the attr cache");
        DeviceConfig got{};
        CHECK(device_shadow_get_config(D, &got), "clear_attrs KEEPS the entry (get_config still true)");
        CHECK(got.occupancy_timeout_s == 42, "clear_attrs KEEPS the config");

        device_shadow_remove(D);
        CHECK(!device_shadow_get_config(D, &got), "remove drops the entry entirely (get_config false)");
        CHECK(device_shadow_get_attrs(D, all, 8) == 0, "removed device has no attrs");

        device_shadow_remove(0x0DEADULL);      // unknown — must not crash
        CHECK(true, "remove(unknown IEEE) is a safe no-op");
        device_shadow_clear_attrs(0x0DEADULL); // unknown — must not crash
        CHECK(true, "clear_attrs(unknown IEEE) is a safe no-op");
    }

    // ── G6: device_shadow_process — pipeline pass-through + STR + _last_seen ─
    {
        printf("\nG6 process: upsert + filter + last_seen\n");
        const uint64_t P = 0x00E5ULL;
        DeviceConfig pc{};
        pc.last_seen_enabled = false;
        device_shadow_set_config(P, &pc);

        ZapDevice dev{};
        dev.ieee_addr = P;

        ZclAttribute a[2]{};
        zcl_attr_set_int(&a[0], "state", 1, VAL_BOOL);
        zcl_attr_set_str(&a[1], "action", "toggle");
        device_shadow_process(&dev, a, 2);

        ShadowAttr o{};
        CHECK(device_shadow_get_attr(P, "state", &o) && o.int_val == 1 && o.val_type == VAL_BOOL,
              "process upserts an INT/BOOL attr");
        CHECK(device_shadow_get_attr(P, "action", &o) && o.val_type == VAL_STR &&
              strcmp(o.str_val, "toggle") == 0, "process upserts a STR attr (string round-trip)");

        // Now enable a filter and confirm the filtered key is dropped.
        DeviceConfig fc{};
        fc.last_seen_enabled = false;
        fc.filtered_count    = 1;
        strncpy(fc.filtered[0], "linkquality", ATTR_KEY_MAX - 1);
        device_shadow_set_config(P, &fc);

        ZclAttribute f[2]{};
        zcl_attr_set_int(&f[0], "battery", 80);
        zcl_attr_set_int(&f[1], "linkquality", 120);
        device_shadow_process(&dev, f, 2);
        CHECK(device_shadow_get_attr(P, "battery", &o) && o.int_val == 80,
              "process keeps an unfiltered attr");
        CHECK(!device_shadow_get_attr(P, "linkquality", &o),
              "process drops a filtered attr (shadow_pipeline_filter)");

        // last_seen injection when enabled.
        const uint64_t Q = 0x00E6ULL;
        DeviceConfig qc{};
        qc.last_seen_enabled = true;
        device_shadow_set_config(Q, &qc);
        ZapDevice qdev{};
        qdev.ieee_addr = Q;
        ZclAttribute qa[1]{};
        zcl_attr_set_int(&qa[0], "state", 1, VAL_BOOL);
        device_shadow_process(&qdev, qa, 1);
        CHECK(device_shadow_get_attr(Q, "_last_seen", &o),
              "process injects synthetic _last_seen when last_seen_enabled");
    }

    // ── G7: process debounce buffer → flush-on-disable ─────────────────────
    {
        printf("\nG7 debounce buffering\n");
        const uint64_t R = 0x00E7ULL;
        device_shadow_set_debounce_ms(R, 1000);   // creates entry, debounce on

        ZapDevice rdev{};
        rdev.ieee_addr = R;
        ZclAttribute ra[1]{};
        zcl_attr_set_int(&ra[0], "brightness", 128);
        device_shadow_process(&rdev, ra, 1);

        ShadowAttr o{};
        CHECK(!device_shadow_get_attr(R, "brightness", &o),
              "debounced attr is buffered in pending, NOT yet visible");

        device_shadow_set_debounce_ms(R, 0);       // flushes pending under old window
        CHECK(device_shadow_get_attr(R, "brightness", &o) && o.int_val == 128,
              "disabling debounce flushes the buffered attr into the cache");
    }

    // ── G8: process occupancy-timer arm/disarm (fire not host-testable) ────
    {
        printf("\nG8 occupancy timer arm/disarm\n");
        const uint64_t S = 0x00E8ULL;
        device_shadow_set_debounce_ms(S, 0);       // create entry (debounce off)
        CHECK(device_shadow_set_occupancy_timeout(S, 30), "set occupancy timeout 30s");

        ZapDevice sdev{};
        sdev.ieee_addr = S;
        ZclAttribute occ[1]{};
        zcl_attr_set_int(&occ[0], "occupancy", 1, VAL_BOOL);
        device_shadow_process(&sdev, occ, 1);      // arms the TTL timer (stub) + upserts

        ShadowAttr o{};
        CHECK(device_shadow_get_attr(S, "occupancy", &o) && o.int_val == 1,
              "occupancy=1 upserted and TTL timer armed without crashing");

        CHECK(device_shadow_set_occupancy_timeout(S, 0), "disable occupancy timeout");
        DeviceConfig got{};
        CHECK(device_shadow_get_config(S, &got) && got.occupancy_timeout_s == 0,
              "occupancy timeout cleared in config");
    }

    // ── G9: pure pipeline helpers (direct, deterministic) ──────────────────
    {
        printf("\nG9 pipeline helpers\n");

        // filter
        DeviceConfig fc{};
        fc.filtered_count = 1;
        strncpy(fc.filtered[0], "linkquality", ATTR_KEY_MAX - 1);
        ZclAttribute in[3]{};
        zcl_attr_set_int(&in[0], "state", 1);
        zcl_attr_set_int(&in[1], "linkquality", 5);
        zcl_attr_set_int(&in[2], "battery", 90);
        ZclAttribute out[3]{};
        uint8_t n = shadow_pipeline_filter(&fc, in, 3, out, 3);
        CHECK(n == 2, "filter drops exactly the filtered key");
        CHECK(keyeq(out[0].key, "state") && keyeq(out[1].key, "battery"),
              "filter preserves survivor order");
        CHECK(shadow_pipeline_filter(&fc, in, 3, out, 1) == 1, "filter honours max_out clamp");

        // throttle
        DeviceConfig tc{};
        tc.throttle_ms = 1000;
        uint32_t last = 0;
        CHECK(shadow_pipeline_throttle_pass(&tc, &last, 5000) == true,  "throttle: first report passes");
        CHECK(shadow_pipeline_throttle_pass(&tc, &last, 5500) == false, "throttle: within window blocked");
        CHECK(shadow_pipeline_throttle_pass(&tc, &last, 6000) == true,  "throttle: window elapsed passes");
        DeviceConfig tc0{};
        tc0.throttle_ms = 0;
        uint32_t l2 = 12345;
        CHECK(shadow_pipeline_throttle_pass(&tc0, &l2, 7) == true, "throttle disabled always passes");

        // debounce bypass
        DeviceConfig dc{};
        dc.debounce_ignore_count = 1;
        strncpy(dc.debounce_ignore[0], "action", ATTR_KEY_MAX - 1);
        PendingState ps{};
        ZclAttribute at_state{};
        zcl_attr_set_int(&at_state, "state", 1);
        CHECK(shadow_pipeline_debounce_bypass(&dc, &ps, &at_state) == -1,
              "bypass: non-ignored key debounces (-1)");
        ZclAttribute at_act{};
        zcl_attr_set_int(&at_act, "action", 1);
        CHECK(shadow_pipeline_debounce_bypass(&dc, &ps, &at_act) == 0,
              "bypass: ignored key not pending → bypass (0)");
        ps.pending[0] = at_act;
        ps.pending_count = 1;
        CHECK(shadow_pipeline_debounce_bypass(&dc, &ps, &at_act) == -1,
              "bypass: ignored key equal to pending → debounce (-1)");
        ZclAttribute at_act2{};
        zcl_attr_set_int(&at_act2, "action", 2);
        CHECK(shadow_pipeline_debounce_bypass(&dc, &ps, &at_act2) == 1,
              "bypass: ignored key differs from pending → replace at idx+1");

        // merge + flush
        PendingState ps2{};
        ZclAttribute m[2]{};
        zcl_attr_set_int(&m[0], "a", 1);
        zcl_attr_set_int(&m[1], "b", 2);
        shadow_pipeline_merge_pending(&dc, &ps2, m, 2);
        CHECK(ps2.pending_count == 2, "merge inserts two new pending entries");
        ZclAttribute m2[1]{};
        zcl_attr_set_int(&m2[0], "a", 9);
        shadow_pipeline_merge_pending(&dc, &ps2, m2, 1);
        CHECK(ps2.pending_count == 2 && ps2.pending[0].int_val == 9,
              "merge updates an existing key in place (last-write-wins)");
        ZclAttribute fo[2]{};
        uint8_t fn = shadow_pipeline_flush_pending(&ps2, fo, 1);
        CHECK(fn == 1 && ps2.pending_count == 1, "flush partial (max_out=1) leaves the remainder");
        CHECK(keyeq(ps2.pending[0].key, "b"), "flush shifts the un-flushed entry to the front");
        uint8_t fn2 = shadow_pipeline_flush_pending(&ps2, fo, 8);
        CHECK(fn2 == 1 && ps2.pending_count == 0, "flush drains the remainder");
    }

    // ── G10: persistence — synchronous config write ────────────────────────
    {
        printf("\nG10 config persisted synchronously\n");
        nvs_stub_reset();
        const uint64_t W = 0x0F10ULL;
        DeviceConfig wc{};
        wc.debounce_ms         = 777;
        wc.last_seen_enabled   = true;
        wc.occupancy_timeout_s = 45;
        device_shadow_set_config(W, &wc);

        DeviceConfig back{};
        CHECK(read_cfg_blob(W, &back), "set_config wrote a 'c' NVS blob synchronously");
        CHECK(back.debounce_ms == 777 && back.occupancy_timeout_s == 45 && back.last_seen_enabled,
              "persisted config bytes match what was set");
    }

    // ── G11: persistence — restore_from_pool rehydrates from NVS ───────────
    {
        printf("\nG11 restore from NVS\n");
        nvs_stub_reset();
        const uint64_t V = 0x0F20ULL;

        DeviceConfig vc{};
        vc.debounce_ms         = 1234;
        vc.last_seen_enabled   = false;
        vc.occupancy_timeout_s = 90;
        vc.optimistic          = true;
        seed_cfg_blob(V, vc, false);

        ShadowAttr sa[2] = {
            make_attr("brightness", VAL_INT,  254, 42),
            make_attr("state",      VAL_BOOL, 1,   42),
        };
        seed_attr_blob(V, sa, 2, false);

        ZapDevice vp{};
        vp.ieee_addr = V;
        uint16_t r = device_shadow_restore_from_pool(&vp, 1);
        CHECK(r == 1, "restore_from_pool returns 1 for one pooled device");

        DeviceConfig vg{};
        CHECK(device_shadow_get_config(V, &vg), "restored device is now known");
        CHECK(vg.debounce_ms == 1234 && vg.occupancy_timeout_s == 90 && vg.optimistic,
              "restore loaded config from the NVS 'c' blob");

        ShadowAttr vo{};
        CHECK(device_shadow_get_attr(V, "brightness", &vo) && vo.int_val == 254 && vo.val_type == VAL_INT,
              "restore loaded attr #1 from the NVS 'a' blob");
        CHECK(device_shadow_get_attr(V, "state", &vo) && vo.int_val == 1 && vo.val_type == VAL_BOOL,
              "restore loaded attr #2 from the NVS 'a' blob");
    }

    // ── G12: persistence — CRC guards reject corrupt blobs on load ─────────
    {
        printf("\nG12 CRC integrity guards\n");

        // Corrupt attr blob → rejected; a valid config beside it still loads.
        nvs_stub_reset();
        const uint64_t X = 0x0F30ULL;
        DeviceConfig xc{};
        xc.debounce_ms       = 55;
        xc.last_seen_enabled = false;
        seed_cfg_blob(X, xc, false);
        ShadowAttr xa = make_attr("level", VAL_INT, 7, 1);
        seed_attr_blob(X, &xa, 1, /*corrupt=*/true);

        ZapDevice xp{};
        xp.ieee_addr = X;
        CHECK(device_shadow_restore_from_pool(&xp, 1) == 1, "restore still returns 1 with a bad attr blob");
        ShadowAttr o{};
        CHECK(!device_shadow_get_attr(X, "level", &o),
              "corrupt attr blob is rejected by the F26 CRC guard (no attrs loaded)");
        DeviceConfig xg{};
        CHECK(device_shadow_get_config(X, &xg) && xg.debounce_ms == 55,
              "the valid config blob beside it still loads");

        // Corrupt config blob → rejected; entry falls back to defaults.
        nvs_stub_reset();
        const uint64_t Y = 0x0F40ULL;
        DeviceConfig yc{};
        yc.debounce_ms = 999;
        seed_cfg_blob(Y, yc, /*corrupt=*/true);
        ZapDevice yp{};
        yp.ieee_addr = Y;
        device_shadow_restore_from_pool(&yp, 1);
        DeviceConfig yg{};
        CHECK(device_shadow_get_config(Y, &yg), "device is created even though its config blob was rejected");
        CHECK(yg.debounce_ms == 0 && yg.last_seen_enabled == true,
              "rejected config → T27 fallback to defaults (debounce 0, last_seen_enabled true)");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS", s_failures);
    return s_failures ? 1 : 0;
}
