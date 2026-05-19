// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "unity.h"
#include "hap_protocol.h"
#include <cstring>

TEST_CASE("HAP encode/decode round-trip CMD type", "[hap]") {
    const char* json = "{\"k\":1}";
    uint8_t buf[HAP_MAX_FRAME_SIZE];

    HapFrame tx{};
    tx.type        = HapMsgType::CMD;
    tx.seq         = 42;
    tx.ack_seq     = 7;
    tx.flags       = 0x01;
    tx.payload     = reinterpret_cast<const uint8_t*>(json);
    tx.payload_len = static_cast<uint16_t>(strlen(json));

    size_t len = hap_encode(tx, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0u, len);

    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode(buf, len, rx));
    TEST_ASSERT_EQUAL(HapMsgType::CMD, rx.type);
    TEST_ASSERT_EQUAL(42, rx.seq);
    TEST_ASSERT_EQUAL(7,  rx.ack_seq);
    TEST_ASSERT_EQUAL(0x01, rx.flags);
    TEST_ASSERT_EQUAL(tx.payload_len, rx.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(json, rx.payload, tx.payload_len);
}

TEST_CASE("HAP ack_seq defaults to 0 for unsolicited frames", "[hap]") {
    uint8_t buf[HAP_MAX_FRAME_SIZE];
    HapFrame tx{};
    tx.type = HapMsgType::HEARTBEAT;
    tx.seq  = 100;
    size_t len = hap_encode(tx, buf, sizeof(buf));
    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode(buf, len, rx));
    TEST_ASSERT_EQUAL(0, rx.ack_seq);
}

TEST_CASE("HAP CRC mismatch detection", "[hap]") {
    const char* json = "{\"v\":0}";
    uint8_t buf[HAP_MAX_FRAME_SIZE];

    HapFrame tx{};
    tx.type        = HapMsgType::EVT;
    tx.seq         = 1;
    tx.payload     = reinterpret_cast<const uint8_t*>(json);
    tx.payload_len = static_cast<uint16_t>(strlen(json));

    size_t len = hap_encode(tx, buf, sizeof(buf));
    buf[len - 1] ^= 0xFF;  // corrupt last CRC byte

    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_CRC_ERROR, hap_decode(buf, len, rx));
}

TEST_CASE("HAP zero-payload frame", "[hap]") {
    uint8_t buf[HAP_MAX_FRAME_SIZE];
    HapFrame tx{};
    tx.type        = HapMsgType::HEARTBEAT;
    tx.seq         = 0;
    tx.payload     = nullptr;
    tx.payload_len = 0;

    size_t len = hap_encode(tx, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(HAP_FRAME_OVERHEAD, len);

    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode(buf, len, rx));
    TEST_ASSERT_EQUAL(0, rx.payload_len);
}

TEST_CASE("HAP buffer overflow returns 0", "[hap]") {
    const char* json = "{\"k\":1}";
    uint8_t tiny[5];  // too small
    HapFrame tx{};
    tx.payload     = reinterpret_cast<const uint8_t*>(json);
    tx.payload_len = static_cast<uint16_t>(strlen(json));
    TEST_ASSERT_EQUAL(0u, hap_encode(tx, tiny, sizeof(tiny)));
}

TEST_CASE("HAP bad preamble rejected", "[hap]") {
    uint8_t buf[HAP_MAX_FRAME_SIZE] = {};
    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;
    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_BAD_MAGIC, hap_decode(buf, HAP_FRAME_OVERHEAD, rx));
}

TEST_CASE("HAP all message type codes round-trip", "[hap]") {
    const HapMsgType types[] = {
        HapMsgType::CMD, HapMsgType::EVT, HapMsgType::ACK,
        HapMsgType::HEARTBEAT, HapMsgType::BULK_STATE_UPDATE,
        HapMsgType::SET_ATTRIBUTE, HapMsgType::DEVICE_EVENT,
    };
    uint8_t buf[HAP_MAX_FRAME_SIZE];
    for (auto t : types) {
        HapFrame tx{};
        tx.type = t;
        size_t len = hap_encode(tx, buf, sizeof(buf));
        HapFrame rx{};
        TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode(buf, len, rx));
        TEST_ASSERT_EQUAL(t, rx.type);
    }
}

TEST_CASE("HAP stream decode skips corrupted prefix", "[hap]") {
    uint8_t buf[3 * HAP_MAX_FRAME_SIZE] = {};
    // 50 bytes of garbage, then a valid frame
    HapFrame tx{};
    tx.type = HapMsgType::HEARTBEAT;
    tx.seq  = 99;
    size_t flen = hap_encode(tx, buf + 50, sizeof(buf) - 50);
    TEST_ASSERT_GREATER_THAN(0u, flen);
    for (size_t i = 0; i < 50; i++) buf[i] = 0xCC;

    HapFrame rx{};
    size_t consumed = 0;
    HapDecodeResult r = hap_decode_stream(buf, 50 + flen, rx, &consumed);
    // Before F-09: returned BAD_MAGIC with consumed=50. That conflated
    // "garbage at offset 0" with "found a valid candidate at offset 50".
    // RESYNC now reports the latter explicitly; callers handle both the
    // same way (skip `consumed`, retry).
    TEST_ASSERT_EQUAL(HAP_DECODE_RESYNC, r);
    TEST_ASSERT_EQUAL(50u, consumed);

    HapDecodeResult r2 = hap_decode_stream(buf + consumed, flen, rx, &consumed);
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, r2);
    TEST_ASSERT_EQUAL(99, rx.seq);
}

TEST_CASE("HAP v3 LEN bit-flip caught by header CRC", "[hap]") {
    uint8_t buf[HAP_MAX_FRAME_SIZE];
    HapFrame tx{};
    tx.type        = HapMsgType::CMD;
    tx.seq         = 1;
    tx.payload_len = 4;
    uint8_t pl[4]  = {1, 2, 3, 4};
    tx.payload     = pl;
    size_t flen    = hap_encode(tx, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0u, flen);

    buf[OFF_LEN] ^= 0x10;
    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_BAD_HDR_CRC, hap_decode(buf, flen, rx));
}

TEST_CASE("HAP v3 payload bit-flip caught by payload CRC, header valid", "[hap]") {
    uint8_t buf[HAP_MAX_FRAME_SIZE];
    HapFrame tx{};
    tx.type        = HapMsgType::CMD;
    tx.seq         = 1;
    tx.payload_len = 8;
    uint8_t pl[8]  = {0x55, 0xAA, 0, 1, 2, 3, 4, 5};
    tx.payload     = pl;
    size_t flen    = hap_encode(tx, buf, sizeof(buf));

    buf[OFF_PAYLOAD + 3] ^= 0x40;
    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_CRC_ERROR, hap_decode(buf, flen, rx));
}

TEST_CASE("v4 stage1 round-trip", "[hap]") {
    HapFrame tx{};
    tx.type        = HapMsgType::CMD;
    tx.seq         = 7;
    tx.ack_seq     = 99;
    tx.flags       = 0x01;
    tx.payload_len = 64;
    uint8_t s1[HAP_STAGE1_LEN];
    hap_encode_stage1(tx, s1);
    HapFrame rx{};
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode_stage1(s1, rx));
    TEST_ASSERT_EQUAL(64, rx.payload_len);
    TEST_ASSERT_EQUAL(HapMsgType::CMD, rx.type);
    TEST_ASSERT_EQUAL(7, rx.seq);
    TEST_ASSERT_EQUAL(99, rx.ack_seq);
    TEST_ASSERT_EQUAL(0x01, rx.flags);
}

// Helper: clock-len math (mirrors the implementation in hap_master.cpp / hap_slave.cpp).
static size_t s2_clock_len_for_test(uint16_t my_pl, uint16_t peer_pl) {
    size_t my_bytes   = (my_pl > 0)   ? (size_t)my_pl + 2   : 0;
    size_t peer_bytes = (peer_pl > 0) ? (size_t)peer_pl + 2 : 0;
    size_t mx = (my_bytes > peer_bytes) ? my_bytes : peer_bytes;
    return (mx + 3) & ~static_cast<size_t>(3);
}

TEST_CASE("v4 stage2_clock_len math", "[hap]") {
    TEST_ASSERT_EQUAL(0u,  s2_clock_len_for_test(0, 0));      // both empty: skip stage 2
    TEST_ASSERT_EQUAL(8u,  s2_clock_len_for_test(0, 4));      // peer 4B + 2 CRC = 6 → round 8
    TEST_ASSERT_EQUAL(8u,  s2_clock_len_for_test(4, 0));      // me 4B + 2 = 6 → 8
    TEST_ASSERT_EQUAL(8u,  s2_clock_len_for_test(4, 4));      // max(6,6)=6 → 8
    TEST_ASSERT_EQUAL(16u, s2_clock_len_for_test(10, 4));     // me 12, peer 6 → max 12 → 12 → 12 (already aligned)
    TEST_ASSERT_EQUAL(20u, s2_clock_len_for_test(16, 0));     // 18 → 20
    TEST_ASSERT_EQUAL(4u,  s2_clock_len_for_test(0, 1));      // 3 → 4
}

TEST_CASE("v4 simulated stage1+stage2 round-trip", "[hap]") {
    // Side A (master) wants to send 4-byte payload; Side B (slave) sends 8-byte payload.
    HapFrame a{};
    a.type        = HapMsgType::CMD;
    a.seq         = 1;
    a.flags       = HAP_FLAG_NEEDS_ACK;
    a.payload_len = 4;
    uint8_t a_pl[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    a.payload     = a_pl;

    HapFrame b{};
    b.type        = HapMsgType::EVT;
    b.seq         = 99;
    b.flags       = HAP_FLAG_NO_ACK;
    b.payload_len = 8;
    uint8_t b_pl[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    b.payload     = b_pl;

    uint8_t a_s1[HAP_STAGE1_LEN], b_s1[HAP_STAGE1_LEN];
    hap_encode_stage1(a, a_s1);
    hap_encode_stage1(b, b_s1);

    // Each side decodes the other's stage 1
    HapFrame got_b{}, got_a{};
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode_stage1(b_s1, got_b));
    TEST_ASSERT_EQUAL(HAP_DECODE_OK, hap_decode_stage1(a_s1, got_a));
    TEST_ASSERT_EQUAL(8, got_b.payload_len);
    TEST_ASSERT_EQUAL(4, got_a.payload_len);

    // Stage-2 size: max(4+2, 8+2) = 10 → round to 12
    size_t s2 = s2_clock_len_for_test(a.payload_len, b.payload_len);
    TEST_ASSERT_EQUAL(12u, s2);

    // Simulate stage-2 buffers (what each side would build)
    uint8_t a_s2_tx[64] = {}, b_s2_tx[64] = {};
    hap_encode_stage2(a, a_s2_tx, sizeof(a_s2_tx));
    hap_encode_stage2(b, b_s2_tx, sizeof(b_s2_tx));

    // Each side verifies the other's stage 2
    TEST_ASSERT_TRUE(hap_verify_stage2(a_s2_tx, a.payload_len));
    TEST_ASSERT_TRUE(hap_verify_stage2(b_s2_tx, b.payload_len));

    // Tamper to verify CRC catches corruption
    a_s2_tx[2] ^= 0xFF;
    TEST_ASSERT_FALSE(hap_verify_stage2(a_s2_tx, a.payload_len));
}
