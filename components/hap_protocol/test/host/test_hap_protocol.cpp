// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host codec coverage for hap_protocol — the (pure, host-buildable) HAP wire
// codec. Exercises both framings + every decode result:
//   • v3 single-frame encode↔decode round-trip (all header fields + payload);
//   • the v3 decode error ladder: TRUNCATED, BAD_MAGIC, BAD_VERSION,
//     BAD_HDR_CRC, OVERFLOW, CRC_ERROR (in the decoder's own check order);
//   • v4 two-stage DMA form: stage-1 header round-trip + errors, stage-2
//     payload encode/verify;
//   • hap_make_reply correlation; hap_decode_stream resync past leading garbage.
#include "hap_protocol.h"

#include <cstdio>
#include <cstring>

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Frame-flags convention (component README §5). hap_protocol.h does not export
// these as named constants — FLAGS is a raw byte — so mirror the wire values.
static constexpr uint8_t HAP_FLAG_NEEDS_ACK = 0x01;
static constexpr uint8_t HAP_FLAG_NO_ACK    = 0x02;

static const uint8_t kPayload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };

// Encode a canonical v3 frame (CMD, seq/ack, NEEDS_ACK, 5-byte payload).
static size_t encode_sample(uint8_t* buf, size_t cap) {
    HapFrame f{};
    f.type        = HapMsgType::CMD;
    f.seq         = 0x1234;
    f.ack_seq     = 0x5678;
    f.flags       = HAP_FLAG_NEEDS_ACK;
    f.payload     = kPayload;
    f.payload_len = sizeof(kPayload);
    return hap_encode(f, buf, cap);
}

int main() {
    // ── v3 encode ↔ decode round-trip ────────────────────────────────────
    {
        uint8_t buf[64];
        size_t n = encode_sample(buf, sizeof buf);
        CHECK(n == HAP_FRAME_OVERHEAD + sizeof(kPayload), "encode writes overhead + payload");
        HapFrame o{};
        CHECK(hap_decode(buf, n, o) == HAP_DECODE_OK, "decode returns OK");
        CHECK(o.type == HapMsgType::CMD && o.seq == 0x1234 && o.ack_seq == 0x5678 &&
              o.flags == HAP_FLAG_NEEDS_ACK && o.payload_len == sizeof(kPayload) &&
              memcmp(o.payload, kPayload, sizeof(kPayload)) == 0,
              "decode round-trips type/seq/ack/flags/payload");
    }
    // empty payload round-trip
    {
        HapFrame f{}; f.type = HapMsgType::ACK; f.seq = 9; f.payload_len = 0;
        uint8_t buf[32]; size_t n = hap_encode(f, buf, sizeof buf);
        HapFrame o{};
        CHECK(n == HAP_FRAME_OVERHEAD && hap_decode(buf, n, o) == HAP_DECODE_OK &&
              o.payload_len == 0 && o.type == HapMsgType::ACK,
              "empty-payload frame round-trips");
    }

    // ── v3 decode error ladder (fresh encode each, corrupt one thing) ────
    { uint8_t b[64]; size_t n = encode_sample(b, sizeof b); HapFrame o{};
      CHECK(hap_decode(b, 10, o) == HAP_DECODE_TRUNCATED, "len < header → TRUNCATED");
      CHECK(hap_decode(b, n - 1, o) == HAP_DECODE_TRUNCATED, "payload cut short → TRUNCATED"); }
    { uint8_t b[64]; size_t n = encode_sample(b, sizeof b); b[0] ^= 0xFF; HapFrame o{};
      CHECK(hap_decode(b, n, o) == HAP_DECODE_BAD_MAGIC, "corrupt preamble → BAD_MAGIC"); }
    { uint8_t b[64]; size_t n = encode_sample(b, sizeof b); b[3] = 0x03; HapFrame o{};
      CHECK(hap_decode(b, n, o) == HAP_DECODE_BAD_VERSION, "wrong version byte → BAD_VERSION"); }
    { uint8_t b[64]; size_t n = encode_sample(b, sizeof b); b[OFF_TYPE] ^= 0xFF; HapFrame o{};
      CHECK(hap_decode(b, n, o) == HAP_DECODE_BAD_HDR_CRC, "corrupt header byte → BAD_HDR_CRC"); }
    { uint8_t b[64]; size_t n = encode_sample(b, sizeof b); b[OFF_PAYLOAD] ^= 0xFF; HapFrame o{};
      CHECK(hap_decode(b, n, o) == HAP_DECODE_CRC_ERROR, "corrupt payload byte → CRC_ERROR"); }
    { // payload_len > HAP_MAX_PAYLOAD → OVERFLOW (encode into a big buffer)
      static uint8_t big[5000] = {0};
      HapFrame f{}; f.type = HapMsgType::STREAM; f.payload = big; f.payload_len = 5000;
      uint8_t b[5100]; size_t n = hap_encode(f, b, sizeof b);
      HapFrame o{};
      CHECK(n > 0 && hap_decode(b, n, o) == HAP_DECODE_OVERFLOW,
            "payload_len > HAP_MAX_PAYLOAD → OVERFLOW"); }

    // ── CRC helpers are pure + sensitive to input ────────────────────────
    {
        const uint8_t d1[] = {1, 2, 3}, d2[] = {1, 2, 4};
        CHECK(hap_crc16(d1, 3) == hap_crc16(d1, 3) && hap_crc16(d1, 3) != hap_crc16(d2, 3),
              "hap_crc16 is deterministic and input-sensitive");
        CHECK(hap_crc8(d1, 3) == hap_crc8(d1, 3) && hap_crc8(d1, 3) != hap_crc8(d2, 3),
              "hap_crc8 is deterministic and input-sensitive");
    }

    // ── v4 stage-1 header round-trip + errors ────────────────────────────
    {
        HapFrame f{}; f.type = HapMsgType::EVT; f.seq = 0xAABB; f.ack_seq = 0xCCDD;
        f.flags = HAP_FLAG_NO_ACK; f.payload_len = 200;
        uint8_t s1[HAP_STAGE1_LEN]; hap_encode_stage1(f, s1);
        HapFrame o{};
        CHECK(hap_decode_stage1(s1, o) == HAP_DECODE_OK && o.type == HapMsgType::EVT &&
              o.seq == 0xAABB && o.ack_seq == 0xCCDD && o.flags == HAP_FLAG_NO_ACK &&
              o.payload_len == 200,
              "stage-1 round-trips the header + stage-2 length");
        uint8_t bad[HAP_STAGE1_LEN]; memcpy(bad, s1, sizeof bad); bad[0] ^= 0xFF;
        HapFrame o2{};
        CHECK(hap_decode_stage1(bad, o2) == HAP_DECODE_BAD_MAGIC, "stage-1 corrupt preamble → BAD_MAGIC");
        memcpy(bad, s1, sizeof bad); bad[OFF_TYPE] ^= 0xFF; HapFrame o3{};
        CHECK(hap_decode_stage1(bad, o3) == HAP_DECODE_BAD_HDR_CRC, "stage-1 corrupt header → BAD_HDR_CRC");
    }

    // ── v4 stage-2 payload encode / verify ───────────────────────────────
    {
        const uint8_t pl[] = {1, 2, 3, 4, 5};
        HapFrame f{}; f.payload = pl; f.payload_len = sizeof(pl);
        uint8_t s2[64]; size_t n = hap_encode_stage2(f, s2, sizeof s2);
        CHECK(n == sizeof(pl) + 2, "stage-2 = payload + CRC16");
        CHECK(hap_verify_stage2(s2, sizeof(pl)), "stage-2 verify passes on a clean buffer");
        s2[0] ^= 0xFF;
        CHECK(!hap_verify_stage2(s2, sizeof(pl)), "stage-2 verify fails on corruption");
    }

    // ── hap_make_reply correlation ───────────────────────────────────────
    {
        HapFrame req{}; req.type = HapMsgType::SET_ATTRIBUTE; req.seq = 0x0777;
        HapFrame rep = hap_make_reply(req, HapMsgType::SET_ACK, HAP_FLAG_NO_ACK);
        CHECK(rep.type == HapMsgType::SET_ACK && rep.ack_seq == 0x0777 &&
              rep.flags == HAP_FLAG_NO_ACK,
              "make_reply echoes req.seq into ack_seq + sets type/flags");
    }

    // ── hap_decode_stream resyncs past leading garbage ───────────────────
    {
        uint8_t frame[64]; size_t fn = encode_sample(frame, sizeof frame);
        // clean frame → OK, consumed == frame length
        { HapFrame o{}; size_t c = 0;
          CHECK(hap_decode_stream(frame, fn, o, &c) == HAP_DECODE_OK && c == fn,
                "decode_stream on a clean frame → OK, consumed = frame length"); }
        // 3 garbage bytes then the frame → consume-and-retry lands on the frame
        uint8_t buf[80]; buf[0] = 0x11; buf[1] = 0x22; buf[2] = 0x33;
        memcpy(buf + 3, frame, fn);
        size_t total = 3 + fn, off = 0; HapFrame o{}; size_t consumed = 0;
        HapDecodeResult r = HAP_DECODE_TRUNCATED; int guard = 0;
        do { r = hap_decode_stream(buf + off, total - off, o, &consumed); off += consumed; }
        while (r != HAP_DECODE_OK && consumed > 0 && guard++ < 6);
        CHECK(r == HAP_DECODE_OK && o.payload_len == sizeof(kPayload) &&
              memcmp(o.payload, kPayload, sizeof(kPayload)) == 0,
              "decode_stream resyncs past garbage and decodes the framed payload");
    }

    printf("%s (%d failure%s)\n", s_failures ? "FAILED" : "OK",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
