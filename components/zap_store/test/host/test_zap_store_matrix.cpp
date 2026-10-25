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
//   • last_seen side-car: survives restart + reset,
//     writes only on change, pre-clock never stored,
//     never lowers a newer record value                 save_last_seen / flush_now / load_devices
//
// Characterization only — asserts the component's ACTUAL behavior; it never
// modifies zap_store. A failing CHECK here means a prediction was wrong (fix
// the test) or the component's contract genuinely drifted (investigate).
#include "zap_store.h"
#include "zap_common.h"
#include "nvs.h"          // in-memory NVS stub API — used to corrupt a blob
#include "esp_ota_ops.h"  // the image's trial state, for the move to zhac_data

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
extern "C" int  nvs_stub_blob_writes(const char* key);   // nvs_set_blob count per key
extern "C" void nvs_stub_add_partition(const char* label);
extern "C" void nvs_stub_drop_partition(const char* label);
extern "C" void nvs_stub_init_result(const char* part, esp_err_t e);
extern "C" void nvs_stub_ota_state(int st);
extern "C" void nvs_stub_cut_after(int writes);           // power cut after n writes; -1 = none
extern "C" void nvs_stub_open_fails(bool f);
extern "C" void nvs_stub_part_fails(const char* part, bool f);   // a partition that cannot be read at all
extern "C" int  nvs_stub_count(const char* part, const char* ns);

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

    // ── G14: last_seen survives a restart and a reset ──────────────────────
    // The radio updates ZapDevice.last_seen in RAM on every frame; the 522 B
    // record reaches flash only with other changes (interview, rename,
    // battery). A reboot used to reload the last_seen of that last rewrite:
    // "seen 2 min ago" before a flash, "seen 30 h ago" after it.
    const uint32_t T0 = 1790000000u;   // 2026-09-21, a clock SNTP has set
    {
        fresh_store();
        g_live.clear();
        ZapDevice x = make_dev(0x0A0AULL, 0x1A1A, "sensor");
        x.last_seen = T0;
        (void)zap_store_save_device(&x);   // the record, as of its last rewrite
        x.last_seen = T0 + 7200;           // heard two hours later: RAM only
        g_live[x.ieee_addr] = x;

        zap_store_flush_now();             // graceful restart / OTA
        ZapDevice pool[4]{};
        uint16_t n = zap_store_load_devices(pool, 4);
        CHECK(n == 1 && pool[0].last_seen == T0 + 7200,
              "restart: last_seen reloads as the RAM value, not the record's");

        // Periodic save, then an ungraceful reset (USB flash, power cut).
        g_live[x.ieee_addr].last_seen = T0 + 9000;
        const int rec0 = nvs_stub_blob_writes("d0000");
        CHECK(zap_store_save_last_seen(test_snapshot), "periodic save returns true");
        n = zap_store_load_devices(pool, 4);
        CHECK(n == 1 && pool[0].last_seen == T0 + 9000,
              "reset: last_seen reloads as of the last periodic save");
        CHECK(nvs_stub_blob_writes("d0000") == rec0,
              "last_seen alone never rewrites the 522 B record");
    }

    // ── G15: the periodic save writes only when a value changed ────────────
    {
        const int w0 = nvs_stub_blob_writes("lseen");
        CHECK(zap_store_save_last_seen(test_snapshot), "save with nothing new returns true");
        CHECK(nvs_stub_blob_writes("lseen") == w0, "nothing new: no flash write");
        g_live[0x0A0AULL].last_seen = T0 + 9060;
        (void)zap_store_save_last_seen(test_snapshot);
        CHECK(nvs_stub_blob_writes("lseen") == w0 + 1, "one device heard: one write");
        (void)zap_store_save_last_seen(test_snapshot);
        CHECK(nvs_stub_blob_writes("lseen") == w0 + 1, "and none after it");
    }

    // ── G16: pre-clock values never reach flash; the side-car only raises ──
    {
        fresh_store();
        g_live.clear();
        ZapDevice y = make_dev(0x0B0BULL, 0x2B2B, "y");     // seen with a set clock
        ZapDevice z = make_dev(0x0C0CULL, 0x3C3C, "z");     // never seen with one
        y.last_seen = T0;
        z.last_seen = 0;
        (void)zap_store_save_device(&y);
        (void)zap_store_save_device(&z);
        g_live[y.ieee_addr] = y;
        g_live[z.ieee_addr] = z;
        g_live[z.ieee_addr].last_seen = 42;                 // 1970 + 42 s: clock not set
        (void)zap_store_save_last_seen(test_snapshot);

        nvs_handle_t h;
        (void)nvs_open("zap_v0", NVS_READWRITE, &h);
        size_t len = 0;
        const bool have = nvs_get_blob(h, "lseen", nullptr, &len) == ESP_OK;
        CHECK(have && len == 12, "side-car holds one 12 B row: the pre-clock device has none");

        ZapDevice pool[4]{};
        uint16_t n = zap_store_load_devices(pool, 4);
        bool z_ok = false;
        for (uint16_t i = 0; i < n; i++) if (pool[i].ieee_addr == z.ieee_addr) z_ok = pool[i].last_seen == 0;
        CHECK(n == 2 && z_ok, "a pre-clock value is never loaded back");

        // A record rewritten after the side-car's save (rename, battery) is newer.
        y.last_seen = T0 + 500;
        (void)zap_store_save_device(&y);
        n = zap_store_load_devices(pool, 4);
        bool y_ok = false;
        for (uint16_t i = 0; i < n; i++) if (pool[i].ieee_addr == y.ieee_addr) y_ok = pool[i].last_seen == T0 + 500;
        CHECK(y_ok, "an older side-car value never lowers a newer record");

        // A side-car of the wrong size is ignored, not half-read.
        const uint8_t junk[5] = {1, 2, 3, 4, 5};
        (void)nvs_set_blob(h, "lseen", junk, sizeof(junk));
        nvs_close(h);
        n = zap_store_load_devices(pool, 4);
        y_ok = false;
        for (uint16_t i = 0; i < n; i++) if (pool[i].ieee_addr == y.ieee_addr) y_ok = pool[i].last_seen == T0 + 500;
        CHECK(n == 2 && y_ok, "a malformed side-car is ignored");
    }

    // ── G17: storage faults never erase (review 2026-10-24 PB-01) ──────────
    {
        fresh_store();
        zap_store_init();
        ZapDevice d = make_dev(0x1717ULL, 0x0717, "keep");
        (void)zap_store_save_device(&d);
        ZapDevice pool[4]{};

        nvs_stub_set_schema(99);                    // written by a newer firmware
        zap_store_init();                           // this one is a downgrade
        CHECK(!zap_store_is_ready(), "newer schema: the store stays shut");
        CHECK(zap_store_load_devices(pool, 4) == 0, "newer schema: nothing loaded");
        CHECK(!zap_store_save_device(&d), "newer schema: saves refused");
        CHECK(nvs_stub_count("nvs", "zap_v0") >= 3, "newer schema: records not erased");
        nvs_stub_set_schema(6);                     // the newer firmware again
        zap_store_init();
        CHECK(zap_store_is_ready() && zap_store_load_devices(pool, 4) == 1,
              "back on the newer firmware: the device is still there");

        nvs_stub_open_fails(true);                  // namespace cannot be opened
        zap_store_init();
        CHECK(!zap_store_is_ready(), "unopenable store: not ready, and init did not abort");
        nvs_stub_open_fails(false);
        zap_store_init();
        CHECK(zap_store_load_devices(pool, 4) == 1, "unopenable store: nothing was erased");
    }

    // ── G18: a delete cut by a power loss loses nothing else (CC-01) ───────
    {
        const uint64_t A = 0xA1ULL, B = 0xB2ULL, C = 0xC3ULL;
        for (int cut = 0; cut <= 3; cut++) {
            fresh_store();
            zap_store_init();
            ZapDevice a = make_dev(A, 1, "A"), b = make_dev(B, 2, "B"), c = make_dev(C, 3, "C");
            (void)zap_store_save_device(&a);
            (void)zap_store_save_device(&b);
            (void)zap_store_save_device(&c);
            nvs_stub_cut_after(cut);
            (void)zap_store_delete_device(A);
            nvs_stub_cut_after(-1);
            zap_store_init();                       // the reboot
            ZapDevice pool[8]{};
            const uint16_t n = zap_store_load_devices(pool, 8);
            char msg[96];
            snprintf(msg, sizeof(msg), "cut after %d write(s): B and C survive", cut);
            CHECK(pool_has(pool, n, B) && pool_has(pool, n, C), msg);
            snprintf(msg, sizeof(msg), "cut after %d write(s): one record per device", cut);
            CHECK(n == (pool_has(pool, n, A) ? 3 : 2), msg);
            if (cut >= 1) {
                snprintf(msg, sizeof(msg), "cut after %d write(s): the delete is finished", cut);
                CHECK(!pool_has(pool, n, A), msg);
            }
            if (cut == 2) {
                // The tail key that cut left behind is reused, not leaked.
                ZapDevice d = make_dev(0xD4ULL, 4, "D");
                CHECK(zap_store_save_device(&d), "save after a delete cut before its last write");
                CHECK(zap_store_load_devices(pool, 8) == 3, "... three devices, no ghost slot");
            }
        }
    }

    // ── G19: the store moves to zhac_data, safe at every cut (WC-04) ───────
    {
        // Two devices and a shadow namespace on nvs, as an installed hub has them.
        auto seed = []() {
            fresh_store();
            zap_store_init();                       // no zhac_data yet: on nvs
            ZapDevice a = make_dev(0x01ULL, 1, "one"), b = make_dev(0x02ULL, 2, "two");
            (void)zap_store_save_device(&a);
            (void)zap_store_save_device(&b);
            nvs_handle_t h;
            (void)nvs_open("zap_shadow", NVS_READWRITE, &h);
            (void)nvs_set_u8(h, "ver", 9);
            const uint8_t blob[3] = {1, 2, 3};
            (void)nvs_set_blob(h, "a00000000000001", blob, sizeof(blob));   // NVS keys: 15 chars max
            nvs_close(h);
        };
        ZapDevice pool[8]{};

        seed();
        CHECK(strcmp(zap_store_data_part(), "nvs") == 0, "without zhac_data the store stays on nvs");

        // An update's first boot: copied, the originals kept for a rollback.
        nvs_stub_add_partition("zhac_data");
        nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
        zap_store_init();
        CHECK(strcmp(zap_store_data_part(), "zhac_data") == 0, "trial boot: store on zhac_data");
        CHECK(zap_store_load_devices(pool, 8) == 2, "trial boot: both devices loaded from the copy");
        CHECK(nvs_stub_count("zhac_data", "zap_shadow") == 2, "trial boot: the shadow was copied too");
        CHECK(nvs_stub_count("nvs", "zap_v0") > 0, "trial boot: the originals are kept");

        // Rolled back: the build before runs on nvs and pairs a third device.
        nvs_stub_drop_partition("zhac_data");
        nvs_stub_ota_state(ESP_OTA_IMG_VALID);
        zap_store_init();
        ZapDevice c = make_dev(0x03ULL, 3, "three");
        (void)zap_store_save_device(&c);
        // Updated again: the new trial copies afresh, so the third device moves too.
        nvs_stub_add_partition("zhac_data");
        nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
        zap_store_init();
        CHECK(zap_store_load_devices(pool, 8) == 3, "re-update after a rollback: nothing lost");

        // Confirmed (next boot): the originals go and the move is final.
        nvs_stub_ota_state(ESP_OTA_IMG_VALID);
        zap_store_init();
        CHECK(nvs_stub_count("nvs", "zap_v0") == 0, "confirmed: the originals on nvs are erased");
        CHECK(zap_store_load_devices(pool, 8) == 3, "confirmed: all devices on zhac_data");
        // From now on zhac_data holds the only copy: an init error never formats it.
        nvs_stub_init_result("zhac_data", ESP_ERR_NVS_NO_FREE_PAGES);
        zap_store_init();
        CHECK(!zap_store_is_ready(), "after the move, an unusable zhac_data shuts the store");
        CHECK(nvs_stub_count("zhac_data", "zap_v0") > 0, "... and is not erased");
        nvs_stub_init_result("zhac_data", ESP_OK);
        zap_store_init();
        CHECK(zap_store_is_ready() && zap_store_load_devices(pool, 8) == 3, "... the devices are back");

        // A power cut anywhere in the copy: the next boot ends with every device.
        bool all_ok = true;
        for (int cut = 0; cut <= 12; cut++) {
            seed();
            nvs_stub_add_partition("zhac_data");
            nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
            nvs_stub_cut_after(cut);
            zap_store_init();
            nvs_stub_cut_after(-1);
            zap_store_init();                       // the reboot, still the trial
            if (zap_store_load_devices(pool, 8) != 2) all_ok = false;
        }
        CHECK(all_ok, "a cut at any write of the copy loses no device");

        // Before the move, an unusable zhac_data is formatted (nothing of ours on it).
        seed();
        nvs_stub_add_partition("zhac_data");
        nvs_stub_init_result("zhac_data", ESP_ERR_NVS_NEW_VERSION_FOUND);
        zap_store_init();
        CHECK(strcmp(zap_store_data_part(), "zhac_data") == 0 && zap_store_load_devices(pool, 8) == 2,
              "before the move, an unusable zhac_data is formatted and used");

        // A second update before any plain reboot: the first one was confirmed
        // at run time (the image marked valid), and what changed since must stay.
        seed();
        nvs_stub_add_partition("zhac_data");
        nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
        zap_store_init();                           // update 1, trial boot: copied
        zap_store_confirm_move();                   // ~20 s later: image valid, move final
        ZapDevice d = make_dev(0x04ULL, 4, "paired since");
        CHECK(zap_store_save_device(&d), "a device paired after the update is saved");
        zap_store_init();                           // update 2, trial boot again, no reboot between
        CHECK(zap_store_load_devices(pool, 8) == 3 && pool_has(pool, 3, 0x04ULL),
              "a second update keeps what changed since the first (no copy from nvs)");
        CHECK(nvs_stub_count("nvs", "zap_v0") == 0, "... and drops the old copy on nvs");

        // After the move, nvs unreadable on an update's trial boot: the store stays.
        nvs_stub_part_fails("nvs", true);
        zap_store_init();
        CHECK(zap_store_is_ready() && zap_store_load_devices(pool, 8) == 3,
              "unreadable nvs on a later trial boot: the store on zhac_data is untouched");
        nvs_stub_part_fails("nvs", false);

        // An older build run after the move left records on nvs: erased, not used.
        {
            nvs_handle_t h;
            (void)nvs_open("zap_v0", NVS_READWRITE, &h);
            (void)nvs_set_u16(h, "cnt", 1);
            nvs_close(h);
        }
        nvs_stub_ota_state(ESP_OTA_IMG_VALID);
        zap_store_init();
        CHECK(nvs_stub_count("nvs", "zap_v0") == 0 && zap_store_load_devices(pool, 8) == 3,
              "leftovers of an older build on nvs are erased; zhac_data stays the store");

        // A re-copy cut short leaves no "done": the next boot copies again
        // rather than trusting half a copy and erasing the originals.
        seed();
        nvs_stub_add_partition("zhac_data");
        nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
        zap_store_init();                           // trial: copied, done set
        nvs_stub_drop_partition("zhac_data");       // rolled back: the build before runs on nvs
        nvs_stub_ota_state(ESP_OTA_IMG_VALID);
        zap_store_init();
        ZapDevice e = make_dev(0x05ULL, 5, "paired on the old build");
        (void)zap_store_save_device(&e);
        nvs_stub_add_partition("zhac_data");        // updated again
        nvs_stub_ota_state(ESP_OTA_IMG_PENDING_VERIFY);
        nvs_stub_cut_after(4);                      // the re-copy fails part way
        zap_store_init();
        nvs_stub_cut_after(-1);
        nvs_stub_ota_state(ESP_OTA_IMG_VALID);      // the image was kept: a plain boot
        zap_store_init();
        CHECK(zap_store_load_devices(pool, 8) == 3 && pool_has(pool, 3, 0x05ULL),
              "a re-copy cut short is redone, not trusted");

        // One-time marks travel with the records.
        seed();
        CHECK(!zap_store_flag_get("repair") && zap_store_flag_set("repair") && zap_store_flag_get("repair"),
              "a store flag is set and read back");
        nvs_stub_add_partition("zhac_data");
        zap_store_init();                           // a plain boot: moved
        CHECK(strcmp(zap_store_data_part(), "zhac_data") == 0 && zap_store_flag_get("repair"),
              "a store flag moves to zhac_data with the device records");

        // Leave the default layout for anything that runs after this block.
        nvs_stub_reset();
        zap_store_init();
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS", s_failures);
    return s_failures ? 1 : 0;
}
