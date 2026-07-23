// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Characterization matrix for zap_store's PUBLIC API, exercised end-to-end
// against the REAL zap_store.cpp + zap_store_flush.cpp linked over an
// in-memory NVS stub (stubs/nvs_stub.cpp, shared shape with rule_store's host
// harness). Where the pre-existing test_zap_store_logic.cpp only mirrors two
// pure predicates (capacity-reject + CRC) WITHOUT linking the component, this
// file drives the actual functions:
//
//   • save / load round-trip + field fidelity          zap_store_save_device / load_devices
//   • save(nullptr) reject                              zap_store_save_device
//   • in-place rewrite of an existing IEEE (no growth)  save_device (found_idx >= 0)
//   • slot allocation + delete swap-with-last compaction delete_device
//   • delete of missing / empty                         delete_device
//   • capacity ceiling (ZAP_MAX_DEVICES) + at-cap rewrite save_device
//   • load_devices input guards + max_count clamp       load_devices
//   • CRC32 corruption → entry skipped on load          load_devices
//   • schema-version mismatch wipe / match survives     zap_store_init
//   • writeback: immediate fallback (no flush task)     mark_dirty
//   • writeback: deferred queue drained by flush_now    mark_dirty / flush_now
//   • writeback: flush_device (targeted + clean no-op)  flush_device
//   • writeback: dropped when snapshot reports gone     flush_now + snapshot cb
//   • uplink selector: absent-key default, round-trip,
//     out-of-range fallback                             zhac_uplink_get / zhac_uplink_set
//
// Characterization only — asserts the component's ACTUAL behavior; it never
// modifies zap_store. A failing CHECK here means a prediction was wrong (fix
// the test) or the component's contract genuinely drifted (investigate).
#include "zap_store.h"
#include "zap_common.h"
#include "nvs.h"          // in-memory NVS stub API — used to corrupt a blob

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <map>
#include <vector>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// Test-side hooks into the NVS stub (defined in stubs/nvs_stub.cpp).
extern "C" void nvs_stub_reset(void);
extern "C" void nvs_stub_set_schema(uint16_t v);

// ── Helpers ────────────────────────────────────────────────────────────────
static ZapDevice make_dev(uint64_t ieee, uint16_t nwk, const char* name) {
    ZapDevice d{};
    d.ieee_addr      = ieee;
    d.nwk_addr       = nwk;
    d.device_type    = 7;
    d.endpoint_count = 1;
    d.endpoints[0]   = 1;
    if (name) snprintf(d.friendly_name, sizeof(d.friendly_name), "%s", name);
    return d;
}

// Wipe NVS AND force zap_store's in-RAM IEEE index to rebuild. There is no
// public index-reset, but zap_store_delete_device() unconditionally clears the
// "index built" flag (even on an empty store), so it doubles as one.
static void fresh_store() {
    nvs_stub_reset();
    (void)zap_store_delete_device(0);   // no-op on empty store; clears s_idx_built
}

static bool pool_has(const ZapDevice* pool, uint16_t n, uint64_t ieee) {
    for (uint16_t i = 0; i < n; i++) if (pool[i].ieee_addr == ieee) return true;
    return false;
}

// ── Deferred-writeback snapshot source ──────────────────────────────────────
// The flush layer stores only the dirty IEEE; at flush time it calls back for
// the current device state. This map is that "live pool".
static std::map<uint64_t, ZapDevice> g_live;
static bool test_snapshot(uint64_t ieee, ZapDevice* out) {
    auto it = g_live.find(ieee);
    if (it == g_live.end()) return false;   // device gone → flush drops it
    *out = it->second;
    return true;
}

int main() {
    printf("test_zap_store_matrix (public API over in-memory NVS)\n");

    // ── G1: init / ready ───────────────────────────────────────────────────
    nvs_stub_reset();
    CHECK(!zap_store_is_ready(), "is_ready() false before init");
    zap_store_init();
    CHECK(zap_store_is_ready(), "is_ready() true after init");

    // ── G2: save + reload round-trip, field fidelity, null reject ──────────
    {
        fresh_store();
        ZapDevice d = make_dev(0xAABBCCDDEEFF0011ULL, 0x1234, "TestBulb");
        CHECK(zap_store_save_device(&d), "save new device returns true");

        ZapDevice pool[5]{};
        uint16_t n = zap_store_load_devices(pool, 5);
        CHECK(n == 1, "load returns exactly the one saved device");
        CHECK(pool_has(pool, n, d.ieee_addr), "saved IEEE present after reload");
        bool fields_ok = false;
        for (uint16_t i = 0; i < n; i++)
            if (pool[i].ieee_addr == d.ieee_addr)
                fields_ok = (pool[i].nwk_addr == 0x1234) &&
                            (strcmp(pool[i].friendly_name, "TestBulb") == 0) &&
                            (pool[i].endpoints[0] == 1);
        CHECK(fields_ok, "nwk_addr + friendly_name + endpoint survive round-trip");

        CHECK(!zap_store_save_device(nullptr), "save(nullptr) returns false");
    }

    // ── G3: existing IEEE is an in-place rewrite (count does not grow) ─────
    {
        fresh_store();
        ZapDevice d = make_dev(0x1111222233334444ULL, 0xA1A1, "v1");
        CHECK(zap_store_save_device(&d), "first save of IEEE X");
        d.nwk_addr = 0xB2B2;
        snprintf(d.friendly_name, sizeof(d.friendly_name), "%s", "v2");
        CHECK(zap_store_save_device(&d), "re-save same IEEE X (update)");

        ZapDevice pool[4]{};
        uint16_t n = zap_store_load_devices(pool, 4);
        CHECK(n == 1, "re-save of same IEEE did NOT create a second slot");
        CHECK(n == 1 && pool[0].nwk_addr == 0xB2B2, "rewrite reflects updated nwk_addr");
    }

    // ── G4: multi-device slot allocation + delete swap-with-last compaction ─
    {
        fresh_store();
        ZapDevice a = make_dev(0x1111111111111111ULL, 0x0A01, "A");
        ZapDevice b = make_dev(0x2222222222222222ULL, 0x0B02, "B");
        ZapDevice c = make_dev(0x3333333333333333ULL, 0x0C03, "C");
        CHECK(zap_store_save_device(&a) && zap_store_save_device(&b) &&
              zap_store_save_device(&c), "save three distinct devices");

        ZapDevice pool[8]{};
        uint16_t n = zap_store_load_devices(pool, 8);
        CHECK(n == 3, "load returns all three");

        CHECK(zap_store_delete_device(b.ieee_addr), "delete middle device B returns true");
        n = zap_store_load_devices(pool, 8);
        CHECK(n == 2, "count drops to two after delete");
        CHECK(pool_has(pool, n, a.ieee_addr), "A survives delete");
        CHECK(pool_has(pool, n, c.ieee_addr), "C survives delete (moved by compaction)");
        CHECK(!pool_has(pool, n, b.ieee_addr), "deleted B is gone");

        CHECK(!zap_store_delete_device(0xDEADBEEFULL), "delete of missing IEEE returns false");
        n = zap_store_load_devices(pool, 8);
        CHECK(n == 2, "failed delete leaves store unchanged");

        CHECK(zap_store_delete_device(a.ieee_addr), "delete A");
        CHECK(zap_store_delete_device(c.ieee_addr), "delete C (store now empty)");
        n = zap_store_load_devices(pool, 8);
        CHECK(n == 0, "store empty after deleting all");
        CHECK(!zap_store_delete_device(a.ieee_addr), "delete on empty store returns false");
    }

    // ── G5: capacity ceiling + at-capacity in-place rewrite ────────────────
    {
        fresh_store();
        bool all_saved = true;
        for (uint16_t i = 0; i < ZAP_MAX_DEVICES; i++) {
            ZapDevice d = make_dev(0x1000ULL + i, (uint16_t)(0x2000 + i), "cap");
            if (!zap_store_save_device(&d)) { all_saved = false; break; }
        }
        CHECK(all_saved, "all ZAP_MAX_DEVICES saves succeed");

        std::vector<ZapDevice> pool(ZAP_MAX_DEVICES);
        uint16_t n = zap_store_load_devices(pool.data(), (uint16_t)pool.size());
        CHECK(n == ZAP_MAX_DEVICES, "load returns a full store (ZAP_MAX_DEVICES)");

        ZapDevice overflow = make_dev(0x9999AAAABBBBCCCCULL, 0x7777, "overflow");
        CHECK(!zap_store_save_device(&overflow), "NEW device at full store is REJECTED");

        ZapDevice existing = make_dev(0x1000ULL + 50, 0x5555, "rewrite-at-cap");
        CHECK(zap_store_save_device(&existing), "EXISTING device still rewrites at capacity");

        n = zap_store_load_devices(pool.data(), (uint16_t)pool.size());
        CHECK(n == ZAP_MAX_DEVICES, "count stays at capacity after rejected + rewrite");
    }

    // ── G6: load_devices input guards + max_count clamp ────────────────────
    {
        fresh_store();
        ZapDevice a = make_dev(0x51ULL, 0x0051, "g6a");
        ZapDevice b = make_dev(0x52ULL, 0x0052, "g6b");
        ZapDevice c = make_dev(0x53ULL, 0x0053, "g6c");
        (void)zap_store_save_device(&a);
        (void)zap_store_save_device(&b);
        (void)zap_store_save_device(&c);

        CHECK(zap_store_load_devices(nullptr, 5) == 0, "load(nullptr) returns 0");
        ZapDevice pool[5]{};
        CHECK(zap_store_load_devices(pool, 0) == 0, "load(max_count=0) returns 0");
        CHECK(zap_store_load_devices(pool, 2) == 2, "max_count clamps result (3 stored, cap 2)");
    }

    // ── G7: CRC32 corruption → corrupted entry skipped on load ─────────────
    {
        fresh_store();
        ZapDevice p = make_dev(0x50ULL, 0x1111, "P");   // slot d0000
        ZapDevice q = make_dev(0x51ULL, 0x2222, "Q");   // slot d0001
        (void)zap_store_save_device(&p);
        (void)zap_store_save_device(&q);

        // Tamper the first blob: change payload + write a wrong CRC, mirroring
        // the on-target test's corruption. load() recomputes CRC (crc32 field
        // excluded) and must reject the mismatched record.
        nvs_handle_t h;
        (void)nvs_open("zap_v0", NVS_READWRITE, &h);
        ZapDevice t{}; size_t sz = sizeof(t);
        (void)nvs_get_blob(h, "d0000", &t, &sz);
        t.nwk_addr = 0x9999;
        t.crc32    = 0xBADBAD00;
        (void)nvs_set_blob(h, "d0000", &t, sizeof(t));
        (void)nvs_commit(h);
        nvs_close(h);

        ZapDevice pool[5]{};
        uint16_t n = zap_store_load_devices(pool, 5);
        CHECK(n == 1, "corrupted record skipped → one valid device loads");
        CHECK(pool_has(pool, n, q.ieee_addr), "the uncorrupted device (Q) still loads");
        CHECK(!pool_has(pool, n, p.ieee_addr), "the corrupted device (P) is dropped");
        bool no_ghost = true;
        for (uint16_t i = 0; i < n; i++) if (pool[i].nwk_addr == 0x9999) no_ghost = false;
        CHECK(no_ghost, "tampered payload (nwk=0x9999) never surfaces");
    }

    // ── G8: schema-version mismatch wipes; matching version survives ───────
    {
        // A) mismatch → wipe
        fresh_store();
        zap_store_init();
        ZapDevice d = make_dev(0x8888ULL, 0x0808, "schemaA");
        (void)zap_store_save_device(&d);
        ZapDevice pool[4]{};
        CHECK(zap_store_load_devices(pool, 4) == 1, "device present before schema bump");
        nvs_stub_set_schema(5);                     // simulate older on-flash layout
        zap_store_init();                           // detects 5 != current → erase_all
        CHECK(zap_store_load_devices(pool, 4) == 0, "schema mismatch wiped the device store");

        // B) matching version across re-init → data survives
        fresh_store();
        zap_store_init();                           // writes current schema_ver
        ZapDevice e = make_dev(0x8889ULL, 0x0809, "schemaB");
        (void)zap_store_save_device(&e);
        zap_store_init();                           // schema matches → NO wipe
        CHECK(zap_store_load_devices(pool, 4) == 1, "matching schema version preserves data");
    }

    // ── G9: writeback immediate fallback (BEFORE flush task exists) ────────
    // With no flush task started, mark_dirty must persist synchronously.
    {
        fresh_store();
        ZapDevice d = make_dev(0x0101ULL, 0x0A0A, "immediate");
        zap_store_mark_dirty(&d, ZAP_PERSIST_HIGH);
        ZapDevice pool[4]{};
        CHECK(zap_store_load_devices(pool, 4) == 1 && pool_has(pool, 1, 0x0101ULL),
              "mark_dirty before flush_init persists immediately");
        zap_store_mark_dirty(nullptr, ZAP_PERSIST_LOW);   // must not crash
        CHECK(true, "mark_dirty(nullptr) is a safe no-op");
    }

    // Install the snapshot source + start the (stubbed, non-running) flush task.
    // From here mark_dirty defers instead of writing through.
    zap_store_set_snapshot_cb(test_snapshot);
    zap_store_flush_init();

    // ── G10: deferred queue is NOT on flash until flush_now drains it ──────
    {
        fresh_store();
        ZapDevice e = make_dev(0x0202ULL, 0x0B0B, "deferred");
        g_live[0x0202ULL] = e;
        zap_store_mark_dirty(&e, ZAP_PERSIST_HIGH);

        ZapDevice pool[4]{};
        CHECK(zap_store_load_devices(pool, 4) == 0,
              "deferred mark_dirty is NOT yet persisted (flush task never runs)");
        zap_store_flush_now();
        CHECK(zap_store_load_devices(pool, 4) == 1 && pool_has(pool, 1, 0x0202ULL),
              "flush_now drains the dirty entry to flash");
    }

    // ── G11: flush_device targets one IEEE; clean IEEE is a true no-op ─────
    {
        fresh_store();
        ZapDevice f = make_dev(0x0303ULL, 0x0C0C, "targeted");
        g_live[0x0303ULL] = f;
        zap_store_mark_dirty(&f, ZAP_PERSIST_LOW);
        CHECK(zap_store_flush_device(0x0303ULL), "flush_device(dirty IEEE) returns true");
        ZapDevice pool[4]{};
        CHECK(zap_store_load_devices(pool, 4) == 1 && pool_has(pool, 1, 0x0303ULL),
              "flush_device persisted the targeted device");
        CHECK(zap_store_flush_device(0x0DEADULL),
              "flush_device(non-dirty IEEE) returns true (nothing pending)");
    }

    // ── G12: snapshot reports the device gone → flush drops it (no write) ──
    {
        fresh_store();
        ZapDevice gone = make_dev(0x0404ULL, 0x0D0D, "gone");
        // deliberately NOT added to g_live → snapshot cb returns false
        zap_store_mark_dirty(&gone, ZAP_PERSIST_HIGH);
        zap_store_flush_now();
        ZapDevice pool[4]{};
        CHECK(zap_store_load_devices(pool, 4) == 0,
              "flush drops the dirty entry when snapshot reports the device gone");
    }

    // ── G13: uplink selector — absent key, round-trip, OOR fallback ────────
    {
        fresh_store();
        CHECK(zhac_uplink_get() == ZHAC_UPLINK_CUSTOM_MQTT,
              "absent uplink key defaults to CUSTOM_MQTT");

        CHECK(zhac_uplink_set(ZHAC_UPLINK_RAINMAKER) == ESP_OK,
              "set(RAINMAKER) returns ESP_OK");
        CHECK(zhac_uplink_get() == ZHAC_UPLINK_RAINMAKER,
              "get() reflects RAINMAKER after set");

        CHECK(zhac_uplink_set(ZHAC_UPLINK_NONE) == ESP_OK,
              "set(NONE) returns ESP_OK");
        CHECK(zhac_uplink_get() == ZHAC_UPLINK_NONE,
              "get() reflects NONE after set");

        CHECK(zhac_uplink_set(ZHAC_UPLINK_CUSTOM_MQTT) == ESP_OK,
              "set(CUSTOM_MQTT) returns ESP_OK");
        CHECK(zhac_uplink_get() == ZHAC_UPLINK_CUSTOM_MQTT,
              "get() reflects CUSTOM_MQTT after set");

        // Out-of-range stored byte (e.g. a downgrade after a future enum
        // value shipped) — write directly through the stub's raw NVS API,
        // bypassing zhac_uplink_set (which can only ever write a valid enum
        // value) to simulate a value this build doesn't know about.
        nvs_handle_t h;
        (void)nvs_open("zap_v0", NVS_READWRITE, &h);
        (void)nvs_set_u8(h, "uplink", 99);
        (void)nvs_commit(h);
        nvs_close(h);
        CHECK(zhac_uplink_get() == ZHAC_UPLINK_CUSTOM_MQTT,
              "out-of-range stored value falls back to CUSTOM_MQTT");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS", s_failures);
    return s_failures ? 1 : 0;
}
