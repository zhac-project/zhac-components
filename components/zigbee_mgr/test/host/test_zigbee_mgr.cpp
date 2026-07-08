// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Characterization harness for the zigbee_mgr component — the coordinator
// manager + ZCL command builder + device pool + interview state machine.
// Exercised end-to-end against the REAL zigbee_mgr sources (all 12 .cpp) over
// a MOCKED ZNP transport (stubs/znp_stub.cpp implements znp_driver.h +
// znp_confirm.h at the structured MtFrame level; znp_driver.cpp is NOT
// compiled). The mock RECORDS every SREQ the manager would send to the CC2652
// and INJECTS AREQ indications back to drive the state machine — see
// stubs/znp_stub.h.
//
// What is covered (all deterministic, single-threaded):
//   G1  zigbee_pool — add / find_by_ieee / find_by_nwk / count, multi-device
//       isolation, mark_dirty after in-place nwk mutation, active-vs-total
//       (soft-remove), zigbee_pool_remove swap-with-last compaction + adapter
//       cache invalidation + not-found, recursive lock/unlock, capacity (full).
//   G2  zigbee_pool_restore_persisted — rehydrate the pool from seeded zap_store.
//   G3  Every ZCL command builder — asserts the recorded AF_DATA_REQUEST /
//       ZDO SREQ carries the right cmd0/cmd1 + ZCL/ZDO payload (cluster,
//       command id, addressing, params, trans_id==TSN). Plus the failure
//       gates: SRSP status!=0, no-SRSP, MAC-confirm timeout, invalid inputs.
//   G4  zigbee_mgr_init — a scripted ZNP responder drives the startup SREQ
//       ladder to success (hw-reset→SYS_RESET_IND, NIB-present skips
//       commissioning, STARTUP_FROM_APP→state=9); asserts the ladder + the
//       captured coordinator IEEE.
//   G5  AREQ state machine — TC_DEV_IND seeds the pool, a second refreshes the
//       nwk, ZDO_LEAVE_IND soft-removes, a rejoin clears the removed flag, and
//       an unexpected SYS_RESET_IND flags a ZNP crash.
//   G6  interview_trigger / flush + configure-queue callable surface.
//
// NOT host-testable (documented — xTaskCreate is a no-op so the worker tasks
// never run): the deferred interview read ladder (task_interview), the
// configure-queue drain/apply with DONE-dedup + exponential backoff
// (task_configure), and the zcl_attr decode task. For the configure queue we
// characterize only the synchronous surface (init idempotency + enqueue).
//
// Characterization only — asserts the component's ACTUAL behavior; it never
// modifies production code. A failing CHECK means a prediction was wrong (fix
// the test) or the contract genuinely drifted (investigate).
#include <cstddef>         // size_t — zigbee_mgr.h uses it without including it
#include <cstdint>
#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "zigbee_configure_queue.h"
#include "zap_common.h"
#include "zap_store.h"
#include "znp_stub.h"      // includes znp_driver.h (MtFrame, MT_SREQ/AREQ, ZNP_*)

#include <cstdio>
#include <cstring>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { s_failures++; printf("  FAIL: %s\n", msg); }            \
    else         { printf("  ok:   %s\n", msg); }                          \
} while (0)

// ── little-endian pack helpers (mirror le16/le64 in the production code) ──
static void put_u16le(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u64le(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF; }

// Drain the global device pool to empty between groups (pool_remove is
// swap-with-last; repeated removal of index 0 clears it).
static void pool_clear() { while (pool_count() > 0) pool_remove(0); }

// Did any recorded SREQ carry this (cmd0,cmd1)? Used for the init ladder.
static bool saw_req(uint8_t c0, uint8_t c1) {
    for (int i = 0; i < znp_stub_req_count(); i++) {
        const ZnpRecordedReq& r = znp_stub_req(i);
        if (r.cmd0 == c0 && r.cmd1 == c1) return true;
    }
    return false;
}

static constexpr uint64_t kCoordIeee = 0x00124B0011223344ULL;
static constexpr uint64_t kJoinIeee  = 0xAABBCCDDEEFF0011ULL;

// Scripted ZNP model that makes zigbee_mgr_init() succeed (see G4).
static bool init_responder(const ZnpRecordedReq& req, MtFrame& srsp) {
    static uint8_t buf[16];
    const uint8_t sys = MT_SREQ(ZNP_SYS);   // 0x21
    const uint8_t zdo = MT_SREQ(ZNP_ZDO);   // 0x25

    if (req.cmd0 == sys && req.cmd1 == 0x04) {          // SYS_GET_EXTADDR
        put_u64le(buf, kCoordIeee);
        znp_stub_set_srsp(srsp, buf, 8);
        return true;
    }
    if (req.cmd0 == sys && req.cmd1 == 0x13) {          // SYS_OSAL_NV_LENGTH
        put_u16le(buf, 116);                            // NIB present → configured
        znp_stub_set_srsp(srsp, buf, 2);
        return true;
    }
    if (req.cmd0 == zdo && req.cmd1 == 0x40) {          // ZDO_STARTUP_FROM_APP
        buf[0] = 0x00;
        znp_stub_set_srsp(srsp, buf, 1);
        const uint8_t state9 = 0x09;                    // → ZDO_STATE_CHANGE_IND=9
        znp_stub_inject_areq(MT_AREQ(ZNP_ZDO), 0xC0, &state9, 1);
        return true;
    }
    buf[0] = 0x00;                                      // default: status OK
    znp_stub_set_srsp(srsp, buf, 1);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
int main() {
    printf("test_zigbee_mgr (coordinator mgr + ZCL builders + pool + state machine)\n");

    zap_store_init();          // in-memory NVS ready (needed by pool restore + leave persist)
    zigbee_pool_init();        // allocate the device pool

    ZapDevice snap;

    // ── G1: zigbee_pool ─────────────────────────────────────────────────
    {
        printf("\nG1 zigbee_pool\n");
        pool_clear();
        CHECK(pool_count() == 0, "pool starts empty");

        ZapDevice* a = pool_add();
        CHECK(a != nullptr, "pool_add returns a slot");
        a->ieee_addr = 0x1111000000000001ULL;
        a->nwk_addr  = 0x1001;
        CHECK(pool_count() == 1, "pool_count == 1 after one add");
        CHECK(zigbee_pool_snapshot(0x1111000000000001ULL, &snap) && snap.nwk_addr == 0x1001,
              "find_by_ieee locates the device (nwk matches)");
        CHECK(zigbee_pool_snapshot_by_nwk(0x1001, &snap) &&
                  snap.ieee_addr == 0x1111000000000001ULL,
              "find_by_nwk locates the device (ieee matches)");
        CHECK(!zigbee_pool_snapshot(0xDEAD, &snap), "find_by_ieee misses absent device");

        // Multi-device isolation.
        ZapDevice* b = pool_add();
        CHECK(b != nullptr, "second pool_add returns a slot");
        b->ieee_addr = 0x2222000000000002ULL;
        b->nwk_addr  = 0x2002;
        CHECK(pool_count() == 2, "pool_count == 2 after two adds");
        CHECK(zigbee_pool_snapshot(0x2222000000000002ULL, &snap) && snap.nwk_addr == 0x2002,
              "second device found independently");
        CHECK(zigbee_pool_snapshot(0x1111000000000001ULL, &snap) && snap.nwk_addr == 0x1001,
              "first device still intact after second add");

        // mark_dirty contract: in-place nwk mutation is invisible to the nwk
        // hash index until zigbee_pool_mark_dirty() forces a rebuild.
        pool_clear();
        ZapDevice* c = pool_add();
        c->ieee_addr = 0x3333000000000003ULL;
        c->nwk_addr  = 0x1001;
        CHECK(zigbee_pool_snapshot_by_nwk(0x1001, &snap), "nwk 0x1001 resolves before mutation");
        uint16_t newnwk = 0x2002;
        zigbee_pool_with_device(0x3333000000000003ULL,
            [](ZapDevice* d, void* ctx) { d->nwk_addr = *static_cast<uint16_t*>(ctx); },
            &newnwk);
        zigbee_pool_mark_dirty();
        CHECK(zigbee_pool_snapshot_by_nwk(0x2002, &snap) &&
                  snap.ieee_addr == 0x3333000000000003ULL,
              "new nwk resolves after mark_dirty");
        CHECK(!zigbee_pool_snapshot_by_nwk(0x1001, &snap),
              "old nwk no longer resolves after mark_dirty");
        CHECK(zigbee_pool_snapshot(0x3333000000000003ULL, &snap) && snap.nwk_addr == 0x2002,
              "find_by_ieee still works (ieee key unchanged) and sees new nwk");

        // Active count vs raw count: a soft-removed (tombstoned) device still
        // occupies a slot but is excluded from pool_count_active().
        pool_clear();
        ZapDevice* d = pool_add();
        d->ieee_addr = 0x4444000000000004ULL;
        d->nwk_addr  = 0x4004;
        CHECK(pool_count() == 1 && pool_count_active() == 1, "fresh device is active");
        zigbee_pool_with_device(0x4444000000000004ULL,
            [](ZapDevice* dev, void*) { zap_dev_mark_removed(dev); }, nullptr);
        CHECK(pool_count() == 1, "soft-remove keeps the slot in pool_count");
        CHECK(pool_count_active() == 0, "soft-remove excludes it from pool_count_active");

        // zigbee_pool_remove(ieee): swap-with-last compaction + adapter cache
        // invalidation.
        pool_clear();
        zhac_stub_reset();
        for (uint64_t i = 1; i <= 3; i++) {
            ZapDevice* e = pool_add();
            e->ieee_addr = 0xA000ULL + i;
            e->nwk_addr  = static_cast<uint16_t>(0xB000 + i);
        }
        CHECK(pool_count() == 3, "three devices added for removal test");
        CHECK(zigbee_pool_remove(0xA001ULL), "zigbee_pool_remove finds+removes head device");
        CHECK(pool_count() == 2, "count drops to 2 after hard-remove");
        CHECK(!zigbee_pool_snapshot(0xA001ULL, &snap), "removed device no longer found");
        CHECK(zigbee_pool_snapshot(0xA003ULL, &snap), "last device survives swap-with-last relocation");
        CHECK(zigbee_pool_snapshot(0xA002ULL, &snap), "middle device survives removal");
        CHECK(zhac_stub_last_invalidated_ieee() == 0xA001ULL,
              "hard-remove invalidates the adapter def-cache for that ieee");
        CHECK(zhac_stub_invalidate_count() == 1, "exactly one cache invalidation on remove");
        CHECK(!zigbee_pool_remove(0xDEADBEEFULL), "zigbee_pool_remove returns false for unknown ieee");

        // Recursive advisory lock is callable and nests through the public API.
        zigbee_pool_lock();
        CHECK(zigbee_pool_snapshot(0xA002ULL, &snap),
              "public lookup works while the advisory lock is held (recursive mutex)");
        zigbee_pool_unlock();

        // Capacity: fill to ZAP_MAX_DEVICES, the next add fails.
        pool_clear();
        bool all_ok = true;
        for (uint16_t i = 0; i < ZAP_MAX_DEVICES; i++) {
            ZapDevice* p = pool_add();
            if (!p) { all_ok = false; break; }
            p->ieee_addr = 0xC0000000ULL + i + 1;
        }
        CHECK(all_ok && pool_count() == ZAP_MAX_DEVICES,
              "pool fills to ZAP_MAX_DEVICES without a null add");
        CHECK(pool_add() == nullptr, "pool_add returns nullptr when full");
        CHECK(zigbee_pool_snapshot(0xC0000000ULL + 1, &snap), "first-of-capacity device findable");
        CHECK(zigbee_pool_snapshot(0xC0000000ULL + ZAP_MAX_DEVICES, &snap),
              "last-of-capacity device findable");
        pool_clear();
    }

    // ── G2: zigbee_pool_restore_persisted ───────────────────────────────
    {
        printf("\nG2 zigbee_pool_restore_persisted\n");
        pool_clear();
        CHECK(zap_store_is_ready(), "zap_store ready for restore");

        ZapDevice s1{};
        s1.ieee_addr = 0x5111000000000011ULL;
        s1.nwk_addr  = 0x0A0A;
        std::strcpy(s1.model_id, "MODEL_A");
        CHECK(zap_store_save_device(&s1), "seed device 1 into NVS");

        ZapDevice s2{};
        s2.ieee_addr = 0x5222000000000022ULL;
        s2.nwk_addr  = 0x0B0B;
        CHECK(zap_store_save_device(&s2), "seed device 2 into NVS");

        uint16_t n = zigbee_pool_restore_persisted();
        CHECK(n >= 2, "restore_persisted loads the seeded devices");
        CHECK(pool_count() == n, "pool_count reflects the restored set");
        CHECK(zigbee_pool_snapshot(0x5111000000000011ULL, &snap) && snap.nwk_addr == 0x0A0A,
              "restored device 1 present with its nwk");
        CHECK(zigbee_pool_snapshot(0x5222000000000022ULL, &snap) && snap.nwk_addr == 0x0B0B,
              "restored device 2 present with its nwk");
        pool_clear();
    }

    // ── G3: ZCL command builders (recorded-frame characterization) ──────
    {
        printf("\nG3 ZCL command builders\n");
        const uint8_t AF  = MT_SREQ(ZNP_AF);    // 0x24  AF_DATA_REQUEST subsystem
        const uint8_t ZDO = MT_SREQ(ZNP_ZDO);   // 0x25

        // On/Off — cluster 0x0006, ZCL [FC=0x01, seq, cmd].
        znp_stub_reset();
        CHECK(zigbee_zcl_on_off(0x1234, 5, 0x01), "on_off returns true (SRSP ok + MAC confirm ok)");
        CHECK(znp_stub_req_count() == 1, "on_off emits exactly one SREQ");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == AF && r.cmd1 == 0x01, "on_off = AF_DATA_REQUEST (cmd0=0x24 cmd1=0x01)");
            CHECK(r.payload_len == 13, "on_off AF payload = 10 header + 3 ZCL");
            CHECK(r.payload[0] == 0x34 && r.payload[1] == 0x12, "dst nwk 0x1234 little-endian");
            CHECK(r.payload[2] == 5, "dst endpoint");
            CHECK(r.payload[3] == 0x01, "src endpoint hardcoded 0x01");
            CHECK(r.payload[4] == 0x06 && r.payload[5] == 0x00, "cluster 0x0006 little-endian");
            CHECK(r.payload[7] == 0x00 && r.payload[8] == 0x0F, "AF options=0, radius=15");
            CHECK(r.payload[9] == 0x03, "AF data length = 3");
            CHECK(r.payload[10] == 0x01, "ZCL frame control 0x01 (cluster-specific)");
            CHECK(r.payload[6] == r.payload[11], "AF trans_id equals ZCL TSN");
            CHECK(r.payload[12] == 0x01, "ZCL command id = On (0x01)");
        }

        // permit_join — ZDO_MGMT_PERMIT_JOIN_REQ.
        znp_stub_reset();
        CHECK(zigbee_permit_join(60), "permit_join returns true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == ZDO && r.cmd1 == 0x36, "permit_join = ZDO cmd 0x36");
            CHECK(r.payload_len == 5, "permit_join payload = 5 bytes");
            CHECK(r.payload[0] == 0x0F, "AddrMode = 0x0F broadcast");
            CHECK(r.payload[1] == 0xFC && r.payload[2] == 0xFF, "DstAddr 0xFFFC little-endian");
            CHECK(r.payload[3] == 60, "duration byte");
            CHECK(r.payload[4] == 0x01, "TCSignificance = 1");
        }

        // ZDO bind / unbind — 23-byte payload; bind cmd1=0x21, unbind cmd1=0x22.
        znp_stub_reset();
        CHECK(zigbee_zdo_bind(0x1234, 0x1122334455667788ULL, 1, 0x0006,
                              0xAABBCCDDEEFF0011ULL, 2), "zdo_bind returns true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == ZDO && r.cmd1 == 0x21, "bind = ZDO cmd 0x21");
            CHECK(r.payload_len == 23, "bind payload = 23 bytes");
            CHECK(r.payload[0] == 0x34 && r.payload[1] == 0x12, "src nwk LE");
            CHECK(r.payload[2] == 0x88 && r.payload[9] == 0x11, "src ieee LE (byte0 / byte7)");
            CHECK(r.payload[10] == 1, "src endpoint");
            CHECK(r.payload[11] == 0x06 && r.payload[12] == 0x00, "cluster 0x0006 LE");
            CHECK(r.payload[13] == 0x03, "dst addr mode = 0x03 (IEEE)");
            CHECK(r.payload[14] == 0x11 && r.payload[21] == 0xAA, "dst ieee LE (byte0 / byte7)");
            CHECK(r.payload[22] == 2, "dst endpoint");
        }
        znp_stub_reset();
        CHECK(zigbee_zdo_unbind(0x1234, 0x1122334455667788ULL, 1, 0x0006,
                                0xAABBCCDDEEFF0011ULL, 2), "zdo_unbind returns true");
        CHECK(znp_stub_last().cmd1 == 0x22, "unbind = ZDO cmd 0x22");

        // leave_req — ZDO_MGMT_LEAVE_REQ, 11-byte payload.
        znp_stub_reset();
        CHECK(zigbee_leave_req(0x1234, 0x1122334455667788ULL), "leave_req returns true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == ZDO && r.cmd1 == 0x34, "leave_req = ZDO cmd 0x34");
            CHECK(r.payload_len == 11, "leave_req payload = 11 bytes");
            CHECK(r.payload[0] == 0x34 && r.payload[1] == 0x12, "nwk LE");
            CHECK(r.payload[2] == 0x88 && r.payload[9] == 0x11, "ieee LE");
            CHECK(r.payload[10] == 0x00, "options byte = 0");
        }

        // Level — cluster 0x0008, ZCL [0x01, seq, 0x04, level, tt_lo, tt_hi].
        znp_stub_reset();
        CHECK(zigbee_zcl_level(0x1234, 3, 128, 0x000A), "level returns true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == AF && r.cmd1 == 0x01, "level = AF_DATA_REQUEST");
            CHECK(r.payload[4] == 0x08 && r.payload[5] == 0x00, "cluster 0x0008 LE");
            CHECK(r.payload_len == 16, "level AF payload = 10 + 6 ZCL");
            CHECK(r.payload[10] == 0x01 && r.payload[12] == 0x04,
                  "ZCL FC 0x01, cmd 0x04 MoveToLevelWithOnOff");
            CHECK(r.payload[13] == 128, "level value");
            CHECK(r.payload[14] == 0x0A && r.payload[15] == 0x00, "transition tenths LE");
        }

        // Color temperature — cluster 0x0300, ZCL [0x01, seq, 0x0A, ct_lo, ct_hi, tt_lo, tt_hi].
        znp_stub_reset();
        CHECK(zigbee_zcl_color_temp(0x1234, 3, 370, 5), "color_temp returns true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[4] == 0x00 && r.payload[5] == 0x03, "cluster 0x0300 LE");
            CHECK(r.payload_len == 17, "color_temp AF payload = 10 + 7 ZCL");
            CHECK(r.payload[12] == 0x0A, "ZCL cmd 0x0A MoveToColorTemperature");
            CHECK(r.payload[13] == (370 & 0xFF) && r.payload[14] == (370 >> 8), "mireds LE");
            CHECK(r.payload[15] == 5 && r.payload[16] == 0, "transition tenths LE");
        }

        // Generic Read Attributes — profile-wide, then manufacturer-specific.
        znp_stub_reset();
        {
            const uint8_t attrs[4] = {0x00, 0x00, 0x01, 0x00};   // 0x0000, 0x0001
            CHECK(zigbee_zcl_read(0x1234, 1, 0x0000, attrs, 2, 0), "zcl_read (profile-wide) true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload_len == 10 + 3 + 4, "read body = 3B header + 2 attrs");
            CHECK(r.payload[10] == 0x00 && r.payload[12] == 0x00, "profile-wide FC 0x00, cmd READ 0x00");
            CHECK(r.payload[13] == 0x00 && r.payload[14] == 0x00 &&
                  r.payload[15] == 0x01 && r.payload[16] == 0x00, "attr id list copied LE");
        }
        znp_stub_reset();
        {
            const uint8_t attrs[2] = {0x00, 0x00};
            CHECK(zigbee_zcl_read(0x1234, 1, 0xFCC0, attrs, 1, 0x115F),
                  "zcl_read (manufacturer-specific) true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x04, "manu FC 0x04 (manufacturer-specific)");
            CHECK(r.payload[11] == 0x5F && r.payload[12] == 0x11, "manu code 0x115F LE after FC");
            CHECK(r.payload[14] == 0x00, "manu cmd = READ 0x00 after TSN");
        }
        znp_stub_reset();
        CHECK(!zigbee_zcl_read(0x1234, 1, 0x0000, nullptr, 0, 0),
              "zcl_read rejects null/zero attr list");
        CHECK(znp_stub_req_count() == 0, "rejected read emits no SREQ");

        // Write Attributes — single attr, profile-wide.
        znp_stub_reset();
        {
            const uint8_t val = 0x01;
            CHECK(zigbee_zcl_write_attr(0x1234, 1, 0x0500, 0x0010, 0x20, &val, 1, 0),
                  "zcl_write_attr true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x00 && r.payload[12] == 0x02, "profile FC 0x00, cmd WRITE 0x02");
            CHECK(r.payload[13] == 0x10 && r.payload[14] == 0x00, "attr id 0x0010 LE");
            CHECK(r.payload[15] == 0x20, "attr type 0x20 (u8)");
            CHECK(r.payload[16] == 0x01, "attr value byte");
        }
        znp_stub_reset();
        CHECK(!zigbee_zcl_write_attr(0x1234, 1, 0x0500, 0x0010, 0x20, nullptr, 0, 0),
              "zcl_write_attr rejects empty value");

        // Configure Reporting — analog type carries the reportable-change field;
        // discrete type omits it; unsupported type is rejected.
        znp_stub_reset();
        CHECK(zigbee_zcl_configure_report(0x1234, 1, 0x0402, 0x0000, 0x29,
                                          10, 300, 50, 0), "configure_report (s16) true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x00 && r.payload[12] == 0x06,
                  "profile FC 0x00, cmd CONFIGURE_REPORTING 0x06");
            CHECK(r.payload[13] == 0x00, "direction 0x00 (device reports to us)");
            CHECK(r.payload[14] == 0x00 && r.payload[15] == 0x00, "attr id 0x0000 LE");
            CHECK(r.payload[16] == 0x29, "attr type 0x29 (s16)");
            CHECK(r.payload[17] == 10 && r.payload[18] == 0, "min interval LE");
            CHECK(r.payload[19] == (300 & 0xFF) && r.payload[20] == (300 >> 8), "max interval LE");
            CHECK(r.payload[21] == 50 && r.payload[22] == 0, "reportable change = 2 bytes for s16");
            // 10 AF header + 3 ZCL header + record[dir1 + attr2 + type1 + min2 + max2 + chg2 = 10] = 23
            CHECK(r.payload_len == 23, "s16 configure body length (2B change field)");
        }
        znp_stub_reset();
        CHECK(zigbee_zcl_configure_report(0x1234, 1, 0x0500, 0x0002, 0x10,
                                          0, 3600, 0, 0), "configure_report (bool) true");
        // Discrete type: record[dir1 + attr2 + type1 + min2 + max2 = 8], no change field.
        CHECK(znp_stub_last().payload_len == 21,
              "bool configure body length (no reportable-change field)");
        znp_stub_reset();
        CHECK(!zigbee_zcl_configure_report(0x1234, 1, 0x0500, 0x0002, 0x99,
                                           0, 3600, 0, 0), "configure_report rejects unsupported type");
        CHECK(znp_stub_req_count() == 0, "rejected configure_report emits no SREQ");

        // Cluster-specific command — FC bit0 set; disable-default-response flag.
        znp_stub_reset();
        {
            const uint8_t pl[2] = {0x11, 0x22};
            CHECK(zigbee_zcl_cluster_command(0x1234, 1, 0x0006, 0x40, pl, 2, 0),
                  "cluster_command true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x01, "FC 0x01 (cluster-specific, no disable-default-resp)");
            CHECK(r.payload[12] == 0x40, "command id 0x40");
            CHECK(r.payload[13] == 0x11 && r.payload[14] == 0x22, "command payload copied");
        }
        znp_stub_reset();
        CHECK(zigbee_zcl_cluster_command(0x1234, 1, 0x0006, 0x40, nullptr, 0, 0x01),
              "cluster_command with disable-default-response flag true");
        CHECK(znp_stub_last().payload[10] == 0x11, "FC 0x11 when kStepFlagDisableDefaultResponse set");
        znp_stub_reset();
        CHECK(zigbee_zcl_cluster_command_wait_confirm(0x1234, 1, 0x0006, 0x40, nullptr, 0, 0, 2000),
              "cluster_command_wait_confirm true (MAC confirm gated)");

        // Tuya magic packet — genBasic read of six fixed attrs.
        znp_stub_reset();
        CHECK(zigbee_tuya_magic_packet(0x1234, 1), "tuya_magic_packet true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[4] == 0x00 && r.payload[5] == 0x00, "cluster 0x0000 (genBasic)");
            CHECK(r.payload[9] == 15, "magic packet ZCL body = 3 + 6 attrs*2 = 15");
            CHECK(r.payload[10] == 0x00 && r.payload[12] == 0x00, "profile READ frame");
            CHECK(r.payload[13] == 0x04 && r.payload[14] == 0x00, "first attr 0x0004 (manufacturerName)");
        }

        // MiBoxer FUT089Z finalize — TWO cluster-specific commands.
        znp_stub_reset();
        CHECK(zigbee_miboxer_fut089z_finalize(0x1234, 1), "miboxer finalize true (both sends ok)");
        CHECK(znp_stub_req_count() == 2, "miboxer finalize emits exactly two SREQs");
        {
            const ZnpRecordedReq& r0 = znp_stub_req(0);
            CHECK(r0.payload[4] == 0x04 && r0.payload[5] == 0x00, "1st = genGroups 0x0004");
            CHECK(r0.payload[10] == 0x01 && r0.payload[12] == 0xF0, "1st FC 0x01, cmd 0xF0 setZones");
            CHECK(r0.payload[13] == 0x08, "zone count = 8");
            CHECK(r0.payload[14] == 0x65 && r0.payload[15] == 0x00 && r0.payload[16] == 0x01,
                  "zone1 → groupId 101 (0x0065) LE + zoneNum 1");
            const ZnpRecordedReq& r1 = znp_stub_req(1);
            CHECK(r1.payload[4] == 0x00 && r1.payload[5] == 0x00, "2nd = genBasic 0x0000");
            CHECK(r1.payload[10] == 0x11 && r1.payload[12] == 0xF0,
                  "2nd FC 0x11 (disable-default-resp), cmd 0xF0 tuyaSetup");
        }

        // Generic pre-built ZCL frame passthrough.
        znp_stub_reset();
        {
            const uint8_t body[2] = {0xAA, 0xBB};
            CHECK(zigbee_af_send_zcl(0x1234, 7, 0xEF00, 0x42, body, 2, true, 0),
                  "af_send_zcl true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.cmd0 == AF && r.cmd1 == 0x01, "af_send_zcl = AF_DATA_REQUEST");
            CHECK(r.payload[2] == 7 && r.payload[3] == 0x01, "dst ep 7, src ep 1");
            CHECK(r.payload[4] == 0x00 && r.payload[5] == 0xEF, "cluster 0xEF00 LE");
            CHECK(r.payload[6] == 0x42, "trans_id passed through verbatim");
            CHECK(r.payload[9] == 2 && r.payload[10] == 0xAA && r.payload[11] == 0xBB,
                  "pre-built body copied verbatim (no ZCL header prepended)");
        }

        // Read Attributes Response for genTime — attr 0x0007 answered with UTC.
        znp_stub_reset();
        {
            const uint8_t req_body[2] = {0x07, 0x00};   // requesting attr 0x0007 (LocalTime)
            CHECK(zigbee_respond_gen_time(0x1234, 1, 0x55, req_body, 2), "respond_gen_time true");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[4] == 0x0A && r.payload[5] == 0x00, "cluster 0x000A (genTime)");
            CHECK(r.payload[6] == 0x55, "AF trans_id mirrors the request TSN");
            CHECK(r.payload[10] == 0x18, "ZCL FC 0x18 (profile, S->C, disable-default-resp)");
            CHECK(r.payload[11] == 0x55, "ZCL TSN mirrors request");
            CHECK(r.payload[12] == 0x01, "cmd READ_ATTR_RESPONSE 0x01");
            CHECK(r.payload[13] == 0x07 && r.payload[14] == 0x00, "attr id 0x0007 echoed");
            CHECK(r.payload[15] == 0x00 && r.payload[16] == 0xE2,
                  "status SUCCESS + type 0xE2 (UTC) — host clock is post-2000");
        }
        znp_stub_reset();
        {
            const uint8_t req_body[2] = {0x00, 0x00};   // unsupported attr
            CHECK(zigbee_respond_gen_time(0x1234, 1, 0x55, req_body, 2),
                  "respond_gen_time true for unsupported attr");
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[13] == 0x00 && r.payload[14] == 0x00 && r.payload[15] == 0x86,
                  "unsupported attr answered UNSUPPORTED_ATTRIBUTE 0x86 (no type/value)");
        }

        // Default Response — profile-wide and manufacturer-specific echo.
        znp_stub_reset();
        CHECK(zigbee_send_default_response(0x1234, 1, 0x0006, /*incoming_fc*/0x00,
                                           /*mfg*/0, /*tsn*/0x33, /*cmd*/0x0A, /*status*/0x00),
              "default_response (profile) true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x18, "out FC 0x18 (disable-default-resp + inverted direction)");
            CHECK(r.payload[11] == 0x33, "TSN echoed");
            CHECK(r.payload[12] == 0x0B, "global cmd 0x0B Default Response");
            CHECK(r.payload[13] == 0x0A && r.payload[14] == 0x00, "acked cmd id + status");
        }
        znp_stub_reset();
        CHECK(zigbee_send_default_response(0x1234, 1, 0xFCC0, /*incoming_fc*/0x04,
                                           /*mfg*/0x115F, /*tsn*/0x44, /*cmd*/0x01, /*status*/0x00),
              "default_response (manufacturer-specific) true");
        {
            const ZnpRecordedReq& r = znp_stub_last();
            CHECK(r.payload[10] == 0x1C, "out FC 0x1C (manu bit carried + disable-default-resp)");
            CHECK(r.payload[11] == 0x5F && r.payload[12] == 0x11, "mfg code 0x115F LE echoed");
            CHECK(r.payload[13] == 0x44 && r.payload[14] == 0x0B, "TSN + Default Response cmd");
        }

        // Failure gates.
        znp_stub_reset();
        znp_stub_set_responder([](const ZnpRecordedReq&, MtFrame& srsp) {
            static const uint8_t fail[1] = {0x01};       // non-zero SRSP status
            znp_stub_set_srsp(srsp, fail, 1);
            return true;
        });
        {
            const uint8_t attrs[2] = {0x00, 0x00};
            CHECK(!zigbee_zcl_read(0x1234, 1, 0x0000, attrs, 1, 0),
                  "builder returns false on non-zero SRSP status");
            CHECK(znp_stub_req_count() == 1, "frame still recorded despite status failure");
        }
        znp_stub_reset();
        znp_stub_set_responder([](const ZnpRecordedReq&, MtFrame&) { return false; });   // no SRSP
        {
            const uint8_t attrs[2] = {0x00, 0x00};
            CHECK(!zigbee_zcl_read(0x1234, 1, 0x0000, attrs, 1, 0),
                  "builder returns false when the radio never replies (no SRSP)");
        }
        znp_stub_reset();
        znp_stub_set_confirm_status(-1);   // MAC confirm timeout
        CHECK(!zigbee_zcl_on_off(0x1234, 5, 0x02),
              "non-idempotent on_off returns false on MAC-confirm timeout");
        CHECK(znp_stub_req_count() == 1, "on_off still sent the single frame (no blind retry)");
        znp_stub_set_confirm_status(0);
    }

    // ── G4: zigbee_mgr_init startup ladder ──────────────────────────────
    {
        printf("\nG4 zigbee_mgr_init\n");
        znp_stub_reset();
        znp_stub_set_responder(init_responder);
        bool init_ok = zigbee_mgr_init();
        CHECK(init_ok, "zigbee_mgr_init succeeds against the scripted ZNP responder");
        CHECK(zigbee_mgr_coordinator_ieee() == kCoordIeee,
              "coordinator IEEE captured from SYS_GET_EXTADDR");
        CHECK(saw_req(MT_SREQ(ZNP_SYS), 0x01), "ladder issued SYS_PING");
        CHECK(saw_req(MT_SREQ(ZNP_SYS), 0x04), "ladder issued SYS_GET_EXTADDR");
        CHECK(saw_req(MT_SREQ(ZNP_SYS), 0x13), "ladder queried NV length (is_configured)");
        CHECK(saw_req(MT_SREQ(ZNP_ZDO), 0x40), "ladder issued ZDO_STARTUP_FROM_APP");
        CHECK(saw_req(MT_SREQ(ZNP_AF),  0x00), "ladder issued AF_REGISTER");
        CHECK(saw_req(MT_SREQ(ZNP_ZDO), 0x3E), "ladder issued ZDO_MSG_CB_REGISTER");
        CHECK(!zigbee_mgr_crashed(), "coordinator not flagged crashed right after init");
    }

    // ── G5: AREQ-driven state machine (handlers now registered) ─────────
    {
        printf("\nG5 AREQ state machine\n");
        znp_stub_reset();       // keeps AREQ registrations, clears request log
        pool_clear();
        const uint16_t active0 = pool_count_active();

        uint8_t tc[11];
        put_u16le(tc, 0x1234);
        put_u64le(tc + 2, kJoinIeee);
        tc[10] = 0x00;
        CHECK(znp_stub_inject_areq(MT_AREQ(ZNP_ZDO), 0xCA, tc, 11),
              "TC_DEV_IND (join) dispatched to a registered handler");
        CHECK(zigbee_pool_snapshot(kJoinIeee, &snap), "join seeds the device into the pool");
        CHECK(snap.nwk_addr == 0x1234, "join records the announced nwk");
        CHECK(snap.interview_state == static_cast<uint8_t>(InterviewState::NONE),
              "seeded device starts in InterviewState::NONE");
        CHECK(pool_count_active() == active0 + 1, "active count +1 after join");

        put_u16le(tc, 0x5678);   // rejoin with a new short address
        znp_stub_inject_areq(MT_AREQ(ZNP_ZDO), 0xCA, tc, 11);
        CHECK(zigbee_pool_snapshot(kJoinIeee, &snap) && snap.nwk_addr == 0x5678,
              "rejoin refreshes the nwk in place");
        CHECK(pool_count_active() == active0 + 1, "rejoin does not allocate a second slot");

        uint8_t lv[12];
        put_u16le(lv, 0x5678);
        put_u64le(lv + 2, kJoinIeee);
        lv[10] = 0x00; lv[11] = 0x00;
        CHECK(znp_stub_inject_areq(MT_AREQ(ZNP_ZDO), 0xC9, lv, 12),
              "ZDO_LEAVE_IND dispatched to on_zdo_leave_ind");
        CHECK(zigbee_pool_snapshot(kJoinIeee, &snap) && (snap.flags & ZAP_DEV_REMOVED),
              "leave soft-removes (ZAP_DEV_REMOVED flag set)");
        CHECK(pool_count_active() == active0, "soft-remove drops the active count");
        CHECK(pool_count() >= 1, "soft-removed device still occupies a slot (survives rejoin)");

        put_u16le(tc, 0x9ABC);   // rejoin after leave
        znp_stub_inject_areq(MT_AREQ(ZNP_ZDO), 0xCA, tc, 11);
        CHECK(zigbee_pool_snapshot(kJoinIeee, &snap) && !(snap.flags & ZAP_DEV_REMOVED),
              "rejoin clears the ZAP_DEV_REMOVED flag");
        CHECK(snap.nwk_addr == 0x9ABC, "rejoin after leave refreshes nwk");
        CHECK(pool_count_active() == active0 + 1, "rejoined device is active again");

        // Unexpected SYS_RESET_IND after init = ZNP crash.
        CHECK(!zigbee_mgr_crashed(), "not crashed before the unexpected reset");
        znp_stub_inject_areq(MT_AREQ(ZNP_SYS), 0x80, nullptr, 0);
        CHECK(zigbee_mgr_crashed(),
              "post-init SYS_RESET_IND flags a ZNP crash (zigbee_mgr_crashed)");
    }

    // ── G6: interview trigger / flush + configure-queue surface ─────────
    {
        printf("\nG6 interview trigger + configure queue surface\n");
        CHECK(!zigbee_interview_trigger(0x0123456789ABCDEFULL),
              "interview_trigger returns false for a device not in the pool");
        CHECK(zigbee_interview_trigger(kJoinIeee),
              "interview_trigger returns true for a pooled device (queued)");
        zigbee_interview_flush_join_queue();
        CHECK(true, "flush_join_queue is callable without crashing");

        zigbee_configure_init();          // already run during init; must be idempotent
        zigbee_configure_enqueue(kJoinIeee);
        CHECK(true, "configure_init idempotent + enqueue callable "
                    "(worker drain/dedup not host-run — integration-shaped)");
    }

    printf("\n%s (failures=%d)\n", s_failures ? "FAILED" : "OK", s_failures);
    return s_failures ? 1 : 0;
}
