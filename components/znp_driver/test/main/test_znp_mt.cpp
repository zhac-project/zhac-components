// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "znp_driver.h"
#include "znp_transport.h"
#include <cstring>

// ── Legacy encode/decode round-trip tests ────────────────────────────────

TEST_CASE("MT encode/decode round-trip SYS_PING", "[znp]") {
    uint8_t buf[MT_MAX_FRAME];
    MtFrame tx{};
    tx.cmd0 = 0x21; tx.cmd1 = 0x01;
    tx.payload = nullptr; tx.payload_len = 0;
    size_t len = mt_encode(tx, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(MT_OVERHEAD, len);
    MtFrame rx{};
    TEST_ASSERT_EQUAL(MT_DECODE_OK, mt_decode(buf, len, rx));
    TEST_ASSERT_EQUAL(0x21, rx.cmd0);
    TEST_ASSERT_EQUAL(0x01, rx.cmd1);
    TEST_ASSERT_EQUAL(0, rx.payload_len);
}

TEST_CASE("MT FCS mismatch detected", "[znp]") {
    uint8_t buf[MT_MAX_FRAME];
    MtFrame tx{};
    tx.cmd0 = 0x41; tx.cmd1 = 0x80;
    uint8_t payload[] = {0x00, 0x02, 0x01, 0x12, 0x00, 0x01};
    tx.payload = payload; tx.payload_len = sizeof(payload);
    size_t len = mt_encode(tx, buf, sizeof(buf));
    buf[len - 1] ^= 0xFF;
    MtFrame rx{};
    TEST_ASSERT_EQUAL(MT_DECODE_FCS_ERROR, mt_decode(buf, len, rx));
}

TEST_CASE("MT encode payload with FCS", "[znp]") {
    uint8_t payload[] = {0x01, 0x00, 0xF8, 0xFF, 0x07};
    uint8_t buf[MT_MAX_FRAME];
    MtFrame tx{};
    tx.cmd0 = 0x2F; tx.cmd1 = 0x08;
    tx.payload = payload; tx.payload_len = sizeof(payload);
    size_t len = mt_encode(tx, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(MT_OVERHEAD + sizeof(payload), len);
    TEST_ASSERT_EQUAL(0xFE, buf[0]);
    TEST_ASSERT_EQUAL(sizeof(payload), buf[1]);
    uint8_t expected_fcs = buf[1] ^ buf[2] ^ buf[3];
    for (size_t i = 0; i < sizeof(payload); i++) expected_fcs ^= payload[i];
    TEST_ASSERT_EQUAL(expected_fcs, buf[len - 1]);
}

TEST_CASE("MT SOF sync — bad SOF rejected", "[znp]") {
    uint8_t buf[] = {0xAA, 0x00, 0x21, 0x01, 0x20};
    MtFrame rx{};
    TEST_ASSERT_EQUAL(MT_DECODE_BAD_SOF, mt_decode(buf, sizeof(buf), rx));
}

TEST_CASE("MT buffer overflow returns 0", "[znp]") {
    uint8_t tiny[3];
    MtFrame tx{}; tx.cmd0 = 0x21; tx.cmd1 = 0x01;
    TEST_ASSERT_EQUAL(0u, mt_encode(tx, tiny, sizeof(tiny)));
}

// ── SRSP matching helper ─────────────────────────────────────────────────
//
// Rules:
//   - rsp_cmd0 must have SRSP type bits (0x60 | subsys)
//   - subsystem (low 5 bits of cmd0) must match between req and rsp
//   - cmd1 must match exactly
// Anything else is NOT a match — including naive equality against req_cmd0,
// which was the bug in the pre-refactor driver.

TEST_CASE("SRSP match: SREQ|SYS 0x21/0x01 ↔ SRSP|SYS 0x61/0x01 matches", "[znp][match]") {
    TEST_ASSERT_TRUE(znp_is_expected_srsp(0x21, 0x01, 0x61, 0x01));
}

TEST_CASE("SRSP match: SREQ|ZDO 0x25/0x04 ↔ SRSP|ZDO 0x65/0x04 matches", "[znp][match]") {
    TEST_ASSERT_TRUE(znp_is_expected_srsp(0x25, 0x04, 0x65, 0x04));
}

TEST_CASE("SRSP match: naive cmd0 equality rejected", "[znp][match]") {
    // SREQ cmd0 == SRSP cmd0 is impossible (top bits differ) — must fail.
    TEST_ASSERT_FALSE(znp_is_expected_srsp(0x21, 0x01, 0x21, 0x01));
}

TEST_CASE("SRSP match: AREQ type bits rejected", "[znp][match]") {
    // An AREQ (0x40|sub) is never a valid SRSP.
    TEST_ASSERT_FALSE(znp_is_expected_srsp(0x21, 0x01, 0x41, 0x01));
}

TEST_CASE("SRSP match: subsystem mismatch rejected", "[znp][match]") {
    // Right type bits, right cmd1, wrong subsystem.
    TEST_ASSERT_FALSE(znp_is_expected_srsp(0x21 /*SYS*/, 0x01,
                                            0x65 /*ZDO*/, 0x01));
}

TEST_CASE("SRSP match: cmd1 mismatch rejected", "[znp][match]") {
    TEST_ASSERT_FALSE(znp_is_expected_srsp(0x21, 0x01, 0x61, 0x02));
}
