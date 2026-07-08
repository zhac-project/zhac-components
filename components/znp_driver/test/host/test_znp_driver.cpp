// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host characterization tests for the znp_driver transport's host-tractable
// units: the MT wire codec (FCS, encode, streaming parser), AREQ dispatch, the
// AF_DATA_CONFIRM ring, and the pure SRSP/sensitive-frame predicates. These
// pin the CURRENT behavior of the REAL production TUs — no production code is
// modified. The SREQ/SRSP worker round-trip and the RX/UART pump are out of
// scope (they need the RX worker task + UART event queue; xTaskCreate is a
// no-op on the host, so the async pump never runs).
//
// Build:
//   cmake -B build -S . && cmake --build build && ctest --test-dir build

#include "znp_internal.h"    // MT codec, MtStreamParser, znp_stats_bump, state
#include "znp_transport.h"   // AREQ subscribe/dispatch, is_expected_srsp, stats
#include "znp_confirm.h"     // AF_DATA_CONFIRM ring

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int s_failures = 0;
static int s_checks   = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++s_checks;                                                            \
        if (!(cond)) {                                                         \
            ++s_failures;                                                      \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);  \
        }                                                                      \
    } while (0)

// ── helpers ─────────────────────────────────────────────────────────────────
static ZnpFrame mkframe(uint8_t c0, uint8_t c1,
                        std::initializer_list<uint8_t> d) {
    ZnpFrame f{};
    f.cmd0 = c0;
    f.cmd1 = c1;
    f.len  = static_cast<uint8_t>(d.size());
    size_t i = 0;
    for (uint8_t b : d) f.data[i++] = b;
    return f;
}

static bool frame_eq(const ZnpFrame& a, const ZnpFrame& b) {
    if (a.cmd0 != b.cmd0 || a.cmd1 != b.cmd1 || a.len != b.len) return false;
    return memcmp(a.data, b.data, a.len) == 0;
}

// Streaming-parser capture ctx.
struct ParseCap {
    int      count = 0;
    ZnpFrame last{};
};
static void parser_cb(const ZnpFrame& f, void* ctx) {
    auto* c = static_cast<ParseCap*>(ctx);
    c->count++;
    c->last = f;
}
static void feed_all(MtStreamParser& p, const uint8_t* buf, size_t n,
                     ParseCap& cap) {
    for (size_t i = 0; i < n; i++) p.feed(buf[i], parser_cb, &cap);
}

// AREQ capture ctx.
struct AreqCap {
    int      count = 0;
    ZnpFrame last{};
};

// ── Group A: MT XOR-8 FCS ────────────────────────────────────────────────────
static void test_fcs() {
    // Empty payload: fcs == len ^ cmd0 ^ cmd1.
    CHECK(znp_mt_fcs(0, 0x21, 0x02, nullptr, 0) == (0x00 ^ 0x21 ^ 0x02),
          "fcs empty payload = len^cmd0^cmd1");

    // Known vector with data.
    const uint8_t d[] = {0x01, 0x02, 0x03};
    uint8_t expect = 0x03 ^ 0x44 ^ 0x00 ^ 0x01 ^ 0x02 ^ 0x03;  // = 0x47
    CHECK(znp_mt_fcs(3, 0x44, 0x00, d, 3) == expect, "fcs data vector");
    CHECK(expect == 0x47, "fcs vector precomputed == 0x47");

    // Sensitivity: flipping any input byte changes the fcs.
    uint8_t base = znp_mt_fcs(3, 0x44, 0x00, d, 3);
    const uint8_t d2[] = {0x01, 0x02, 0x04};
    CHECK(znp_mt_fcs(3, 0x44, 0x00, d2, 3) != base, "fcs sensitive to data");
    CHECK(znp_mt_fcs(3, 0x45, 0x00, d, 3) != base, "fcs sensitive to cmd0");
    CHECK(znp_mt_fcs(3, 0x44, 0x01, d, 3) != base, "fcs sensitive to cmd1");
    CHECK(znp_mt_fcs(4, 0x44, 0x00, d, 3) != base, "fcs sensitive to len field");
}

// ── Group B: znp_mt_encode ──────────────────────────────────────────────────
static void test_encode() {
    uint8_t buf[MT_MAX_FRAME];

    // Empty payload: [SOF][00][cmd0][cmd1][fcs], total == MT_OVERHEAD.
    ZnpFrame e = mkframe(0x21, 0x02, {});
    size_t n = znp_mt_encode(e, buf, sizeof(buf));
    CHECK(n == MT_OVERHEAD, "encode empty payload total == MT_OVERHEAD");
    CHECK(buf[0] == MT_SOF, "encode SOF byte");
    CHECK(buf[1] == 0x00, "encode len byte 0");
    CHECK(buf[2] == 0x21 && buf[3] == 0x02, "encode cmd bytes");
    CHECK(buf[4] == znp_mt_fcs(0, 0x21, 0x02, nullptr, 0), "encode fcs (empty)");

    // With payload.
    ZnpFrame f = mkframe(0x24, 0x00, {0xAA, 0xBB, 0xCC, 0xDD});
    n = znp_mt_encode(f, buf, sizeof(buf));
    CHECK(n == MT_OVERHEAD + 4, "encode total == overhead + len");
    CHECK(buf[0] == MT_SOF && buf[1] == 4 && buf[2] == 0x24 && buf[3] == 0x00,
          "encode header with payload");
    CHECK(buf[4] == 0xAA && buf[5] == 0xBB && buf[6] == 0xCC && buf[7] == 0xDD,
          "encode payload bytes");
    CHECK(buf[8] == znp_mt_fcs(4, 0x24, 0x00, f.data, 4), "encode trailing fcs");

    // Buffer too small → 0 (total > buf_size), including the one-byte-short case.
    CHECK(znp_mt_encode(f, buf, 4) == 0, "encode rejects tiny buffer");
    CHECK(znp_mt_encode(f, buf, MT_OVERHEAD + 4 - 1) == 0,
          "encode rejects one-byte-short buffer");
    // Exact-fit buffer succeeds.
    CHECK(znp_mt_encode(f, buf, MT_OVERHEAD + 4) == MT_OVERHEAD + 4,
          "encode accepts exact-fit buffer");
}

// ── Group C: encode ↔ streaming-parse round-trip ─────────────────────────────
static void test_roundtrip() {
    uint8_t buf[MT_MAX_FRAME];

    // Normal frame, fed byte-by-byte → exactly one frame, fields preserved.
    {
        ZnpFrame f = mkframe(0x24, 0x81, {0x10, 0x20, 0x30, 0x40, 0x50});
        size_t n = znp_mt_encode(f, buf, sizeof(buf));
        MtStreamParser p; p.reset();
        ParseCap cap;
        feed_all(p, buf, n, cap);
        CHECK(cap.count == 1, "roundtrip emits exactly one frame");
        CHECK(frame_eq(cap.last, f), "roundtrip frame fields preserved");
    }
    // Empty payload round-trip.
    {
        ZnpFrame f = mkframe(0x61, 0x02, {});
        size_t n = znp_mt_encode(f, buf, sizeof(buf));
        MtStreamParser p; p.reset();
        ParseCap cap;
        feed_all(p, buf, n, cap);
        CHECK(cap.count == 1 && frame_eq(cap.last, f),
              "roundtrip empty payload");
    }
    // Max-length payload (len == ZNP_MAX_DATA_LEN == 250) round-trip.
    {
        ZnpFrame f{};
        f.cmd0 = 0x44; f.cmd1 = 0x00; f.len = ZNP_MAX_DATA_LEN;
        for (size_t i = 0; i < ZNP_MAX_DATA_LEN; i++)
            f.data[i] = static_cast<uint8_t>(i & 0xFF);
        size_t n = znp_mt_encode(f, buf, sizeof(buf));
        CHECK(n == MT_OVERHEAD + ZNP_MAX_DATA_LEN, "encode max payload total");
        MtStreamParser p; p.reset();
        ParseCap cap;
        feed_all(p, buf, n, cap);
        CHECK(cap.count == 1 && frame_eq(cap.last, f),
              "roundtrip max-length payload");
    }
}

// ── Group D: streaming-parser robustness / resync ────────────────────────────
static void test_parser_robustness() {
    uint8_t buf[MT_MAX_FRAME];
    ZnpFrame good = mkframe(0x45, 0x01, {0xDE, 0xAD, 0xBE, 0xEF});
    size_t gn = znp_mt_encode(good, buf, sizeof(buf));

    // Leading non-SOF garbage is skipped; the following valid frame is emitted.
    {
        MtStreamParser p; p.reset();
        ParseCap cap;
        const uint8_t junk[] = {0x00, 0x11, 0x22, 0x33};  // none == MT_SOF
        feed_all(p, junk, sizeof(junk), cap);
        CHECK(cap.count == 0, "garbage before SOF emits nothing");
        feed_all(p, buf, gn, cap);
        CHECK(cap.count == 1 && frame_eq(cap.last, good),
              "parser resyncs to first SOF after garbage");
    }

    // Bad FCS → dropped + bad_frames stat bumped; parser then resyncs.
    {
        MtStreamParser p; p.reset();
        ParseCap cap;
        uint8_t bad[MT_MAX_FRAME];
        memcpy(bad, buf, gn);
        bad[gn - 1] ^= 0xFF;  // corrupt only the FCS byte
        uint32_t before = znp_get_stats().bad_frames;
        feed_all(p, bad, gn, cap);
        CHECK(cap.count == 0, "bad-FCS frame is not emitted");
        CHECK(znp_get_stats().bad_frames == before + 1,
              "bad-FCS frame bumps bad_frames stat");
        // Same parser instance must accept a subsequent good frame.
        feed_all(p, buf, gn, cap);
        CHECK(cap.count == 1 && frame_eq(cap.last, good),
              "parser resyncs after a bad-FCS frame");
    }

    // Truncated input (stops mid-data) emits nothing.
    {
        MtStreamParser p; p.reset();
        ParseCap cap;
        feed_all(p, buf, gn - 2, cap);  // drop last data byte + fcs
        CHECK(cap.count == 0, "truncated frame emits nothing");
    }

    // Over-length LEN byte (> ZNP_MAX_DATA_LEN) is rejected; parser resyncs.
    {
        MtStreamParser p; p.reset();
        ParseCap cap;
        const uint8_t overlen[] = {MT_SOF, 251, 0xAA, 0xBB};  // 251 > 250
        feed_all(p, overlen, sizeof(overlen), cap);
        CHECK(cap.count == 0, "over-length LEN byte drops the frame");
        feed_all(p, buf, gn, cap);
        CHECK(cap.count == 1, "parser resyncs after over-length LEN");
    }

    // Doubled SOF: second 0xFE is consumed as a (254 > 250) LEN → reset; a
    // following valid frame still parses.
    {
        MtStreamParser p; p.reset();
        ParseCap cap;
        const uint8_t two_sof[] = {MT_SOF, MT_SOF};
        feed_all(p, two_sof, sizeof(two_sof), cap);
        CHECK(cap.count == 0, "doubled SOF emits nothing");
        feed_all(p, buf, gn, cap);
        CHECK(cap.count == 1, "parser recovers after doubled SOF");
    }
}

// ── Group E: znp_is_expected_srsp (pure predicate) ───────────────────────────
static void test_is_expected_srsp() {
    // SREQ SYS PING (0x21,0x01) → SRSP SYS PING (0x61,0x01).
    CHECK(znp_is_expected_srsp(0x21, 0x01, 0x61, 0x01),
          "matching SRSP accepted");
    // Wrong type bits: AREQ (0x41) is not an SRSP.
    CHECK(!znp_is_expected_srsp(0x21, 0x01, 0x41, 0x01),
          "AREQ rejected as SRSP");
    // Subsystem mismatch: req SYS(1) vs rsp ZDO(5) → 0x65.
    CHECK(!znp_is_expected_srsp(0x21, 0x01, 0x65, 0x01),
          "subsystem mismatch rejected");
    // cmd1 mismatch.
    CHECK(!znp_is_expected_srsp(0x21, 0x01, 0x61, 0x02),
          "cmd1 mismatch rejected");
    // Another valid subsystem: AF (0x24 SREQ → 0x64 SRSP).
    CHECK(znp_is_expected_srsp(0x24, 0x00, 0x64, 0x00),
          "matching AF SRSP accepted");
}

// ── Group F: znp_wire_is_sensitive (network-key redaction predicate) ─────────
static void test_wire_is_sensitive() {
    const uint8_t precfgkey[]  = {0x62, 0x00};  // 0x0062 PRECFGKEY (LE)
    const uint8_t active_key[] = {0x3A, 0x00};  // 0x003A NWK_ACTIVE_KEY_INFO
    const uint8_t altern_key[] = {0x3B, 0x00};  // 0x003B NWK_ALTERN_KEY_INFO
    const uint8_t plain_item[] = {0x01, 0x00};  // 0x0001 non-key item
    const uint8_t short_item[] = {0x62};        // 1 byte

    // SYS NV_WRITE (cmd1=0x09) of PRECFGKEY → sensitive.
    CHECK(znp_wire_is_sensitive(0x21, 0x09, precfgkey, 2),
          "NV_WRITE PRECFGKEY is sensitive");
    // SYS NV_WRITE_EXT (cmd1=0x1D) of NWK_ACTIVE_KEY_INFO → sensitive.
    CHECK(znp_wire_is_sensitive(0x21, 0x1D, active_key, 2),
          "NV_WRITE_EXT active-key-info is sensitive");
    // NWK_ALTERN_KEY_INFO → sensitive.
    CHECK(znp_wire_is_sensitive(0x21, 0x09, altern_key, 2),
          "NV_WRITE altern-key-info is sensitive");
    // Non-key NV item → not sensitive.
    CHECK(!znp_wire_is_sensitive(0x21, 0x09, plain_item, 2),
          "NV_WRITE of a non-key item is not sensitive");
    // Wrong subsystem (AF, subsys 4) → not sensitive.
    CHECK(!znp_wire_is_sensitive(0x24, 0x09, precfgkey, 2),
          "non-SYS subsystem not sensitive");
    // Wrong cmd1 (not an NV write) → not sensitive.
    CHECK(!znp_wire_is_sensitive(0x21, 0x08, precfgkey, 2),
          "non-NV-write cmd1 not sensitive");
    // Too-short payload (< 2 bytes) → not sensitive.
    CHECK(!znp_wire_is_sensitive(0x21, 0x09, short_item, 1),
          "short payload not sensitive");
}

// ── Group G: AREQ dispatch ───────────────────────────────────────────────────
static void test_areq_dispatch() {
    // Subscribe a recorder for (0x45,0x81); a matching dispatch fires it with
    // the exact payload. (0x44/0x80 is reserved by the confirm ring — avoid it.)
    AreqCap a{};
    znp_subscribe_areq(0x45, 0x81, [&a](const ZnpFrame& f) {
        a.count++; a.last = f;
    });
    ZnpFrame m = mkframe(0x45, 0x81, {0x01, 0x02, 0x03});
    znp_areq_dispatch(m);
    CHECK(a.count == 1, "matching AREQ fires subscriber once");
    CHECK(frame_eq(a.last, m), "AREQ subscriber receives exact payload");

    // Non-matching cmd1 does not fire it.
    a.count = 0;
    znp_areq_dispatch(mkframe(0x45, 0x82, {0x09}));
    CHECK(a.count == 0, "non-matching cmd1 does not fire subscriber");

    // A second, distinct subscriber (0x46,0x01): each fires only for its pair.
    AreqCap b{};
    znp_subscribe_areq(0x46, 0x01, [&b](const ZnpFrame& f) {
        b.count++; b.last = f;
    });
    a.count = 0; b.count = 0;
    znp_areq_dispatch(mkframe(0x46, 0x01, {0xAA}));
    CHECK(a.count == 0 && b.count == 1, "distinct subscribers route by (cmd0,cmd1)");
    a.count = 0; b.count = 0;
    znp_areq_dispatch(mkframe(0x45, 0x81, {0xBB}));
    CHECK(a.count == 1 && b.count == 0, "first subscriber still routes correctly");

    // Dispatch to an unsubscribed pair: no crash, nothing fires.
    a.count = 0; b.count = 0;
    znp_areq_dispatch(mkframe(0x99, 0x99, {0x00}));
    CHECK(a.count == 0 && b.count == 0, "unsubscribed pair fires nothing");

    // Re-subscribing the SAME (cmd0,cmd1) REPLACES (last-wins) — the impl
    // dedups by key. NOTE: the public header comment claims "all fire" for
    // duplicate registrations; the code does not. This pins the real behavior.
    AreqCap a2{};
    znp_subscribe_areq(0x45, 0x81, [&a2](const ZnpFrame& f) {
        a2.count++; a2.last = f;
    });
    a.count = 0; a2.count = 0;
    znp_areq_dispatch(mkframe(0x45, 0x81, {0xCC}));
    CHECK(a2.count == 1 && a.count == 0,
          "re-subscribe same pair replaces (last-wins), not additive");
}

// ── Group H: AF_DATA_CONFIRM ring ────────────────────────────────────────────
static ZnpFrame confirm_frame(uint8_t status, uint8_t endpoint, uint8_t tid) {
    return mkframe(0x44, 0x80, {status, endpoint, tid});
}

static void test_confirm_ring() {
    // Ring-full first (clean, empty ring): exactly 16 reservations succeed and
    // the 17th returns the -1 sentinel. Then release everything.
    {
        int slots[20];
        int got = 0;
        for (int i = 0; i < 20; i++) {
            slots[i] = znp_confirm_reserve(static_cast<uint8_t>(0x10 + i));
            if (slots[i] >= 0) got++;
        }
        CHECK(got == 16, "ring holds exactly 16 slots");
        CHECK(slots[16] == -1 && slots[19] == -1,
              "reserve past capacity returns -1 sentinel");
        for (int i = 0; i < 20; i++)
            if (slots[i] >= 0) znp_confirm_release(slots[i]);
    }

    // Basic lifecycle: reserve → valid slot; release frees it.
    {
        int s = znp_confirm_reserve(0x01);
        CHECK(s >= 0 && s < 16, "reserve returns a valid slot handle");
        znp_confirm_release(s);
        int s2 = znp_confirm_reserve(0x02);
        CHECK(s2 >= 0, "reserve after release succeeds");
        znp_confirm_release(s2);
    }

    // Delivery: reserve, inject a matching AF_DATA_CONFIRM, wait → status byte.
    {
        int s = znp_confirm_reserve(0x55);
        CHECK(s >= 0, "reserve for delivery test");
        znp_areq_dispatch(confirm_frame(0x00, 0x01, 0x55));
        CHECK(znp_confirm_wait(s, 100) == 0x00, "wait returns success status 0x00");

        int s2 = znp_confirm_reserve(0x66);
        znp_areq_dispatch(confirm_frame(0xF0, 0x01, 0x66));  // MacTransactionExpired
        CHECK(znp_confirm_wait(s2, 100) == 0xF0, "wait returns status 0xF0");
    }

    // Timeout: no confirm arrives → wait returns -1 (host stub is non-blocking,
    // so this returns immediately without hanging). Slot is freed on return.
    {
        int s = znp_confirm_reserve(0x77);
        CHECK(znp_confirm_wait(s, 10) == -1, "wait without confirm returns -1");
    }

    // Wrong trans_id confirm does not signal the waiter.
    {
        int s = znp_confirm_reserve(0x88);
        znp_areq_dispatch(confirm_frame(0x00, 0x01, 0x99));  // different tid
        CHECK(znp_confirm_wait(s, 10) == -1,
              "confirm for a different trans_id does not signal");
    }

    // Short confirm payload (len < 3) is ignored.
    {
        int s = znp_confirm_reserve(0xA1);
        znp_areq_dispatch(mkframe(0x44, 0x80, {0x00}));  // len 1
        CHECK(znp_confirm_wait(s, 10) == -1, "short AF_DATA_CONFIRM is ignored");
    }

    // Release-after-confirm then re-reserve yields a clean slot (the stale
    // give is drained on reserve; no stale success leaks into the next wait).
    {
        int s = znp_confirm_reserve(0xB1);
        znp_areq_dispatch(confirm_frame(0x00, 0x01, 0xB1));  // gives the slot sem
        znp_confirm_release(s);                              // abandon, no wait
        int s2 = znp_confirm_reserve(0xB2);
        CHECK(znp_confirm_wait(s2, 10) == -1,
              "re-reserved slot does not carry a stale confirm");
    }

    // Out-of-range slot arguments are handled without crashing.
    {
        CHECK(znp_confirm_wait(-1, 10) == -1, "wait(-1) returns -1");
        CHECK(znp_confirm_wait(16, 10) == -1, "wait(16) out of range returns -1");
        CHECK(znp_confirm_wait(1000, 10) == -1, "wait(1000) returns -1");
        znp_confirm_release(-1);      // no-op, must not crash
        znp_confirm_release(16);      // no-op
        znp_confirm_release(1000);    // no-op
        // Ring still usable afterwards.
        int s = znp_confirm_reserve(0xC1);
        CHECK(s >= 0, "ring still functional after invalid-arg calls");
        znp_confirm_release(s);
    }
}

// ── Group I: linked state/stats support TU (znp_state.cpp) ────────────────────
static void test_state_support() {
    // Counter bumps land on the right field.
    uint32_t t0 = znp_get_stats().timeouts;
    znp_stats_bump(ZnpStat::Timeout);
    CHECK(znp_get_stats().timeouts == t0 + 1, "Timeout stat increments");
    uint32_t r0 = znp_get_stats().rx_areq_count;
    znp_stats_bump(ZnpStat::RxAreq);
    CHECK(znp_get_stats().rx_areq_count == r0 + 1, "RxAreq stat increments");

    // State machine: first-boot reset transitions Init → Up.
    znp_state_set(ZnpTransportState::Init);
    CHECK(znp_get_state() == ZnpTransportState::Init, "state set to Init");
    znp_state_note_reset();
    CHECK(znp_get_state() == ZnpTransportState::Up, "first-boot reset → Up");
    // An unexpected reset while Up → Recovering; a successful call clears it.
    znp_state_note_reset();
    CHECK(znp_get_state() == ZnpTransportState::Recovering,
          "unexpected reset while Up → Recovering");
    znp_state_note_ok();
    CHECK(znp_get_state() == ZnpTransportState::Up, "note_ok clears Recovering");
}

int main() {
    test_fcs();
    test_encode();
    test_roundtrip();
    test_parser_robustness();
    test_is_expected_srsp();
    test_wire_is_sensitive();
    test_areq_dispatch();
    test_confirm_ring();
    test_state_support();

    printf("znp_driver host tests: %d checks, %d failure(s)\n",
           s_checks, s_failures);
    if (s_failures) {
        printf("FAILED\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
