// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "hap_protocol.h"
#include <cstring>

uint16_t hap_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

uint8_t hap_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

// v3 single-frame path — see header banner; no live callers (tests only).
size_t hap_encode(const HapFrame& f, uint8_t* buf, size_t buf_size) {
    // Q16 (QWEN_FINDINGS triage): reject a frame that declares payload_len > 0
    // but supplies no payload pointer — otherwise the header length says N while
    // the wire bytes + CRC cover uninitialised buf memory (a corrupt frame that
    // still passes its own CRC). payload_len == 0 is fine: hap_crc16 over 0 bytes
    // is the deterministic 0xFFFF seed, with no uninitialised read.
    if (f.payload_len > 0 && !f.payload) return 0;
    size_t total = HAP_FRAME_OVERHEAD + f.payload_len;
    if (total > buf_size) return 0;

    memcpy(buf + OFF_PREAMBLE, HAP_PREAMBLE, 4);
    buf[OFF_TYPE]      = static_cast<uint8_t>(f.type);
    buf[OFF_FLAGS]     = f.flags;
    buf[OFF_SEQ]       = f.seq & 0xFF;
    buf[OFF_SEQ + 1]   = (f.seq >> 8) & 0xFF;
    buf[OFF_ACK_SEQ]   = f.ack_seq & 0xFF;
    buf[OFF_ACK_SEQ + 1] = (f.ack_seq >> 8) & 0xFF;
    buf[OFF_LEN]       = f.payload_len & 0xFF;
    buf[OFF_LEN + 1]   = (f.payload_len >> 8) & 0xFF;
    buf[OFF_HDR_CRC]   = hap_crc8(buf, OFF_HDR_CRC);

    if (f.payload_len > 0 && f.payload)
        memcpy(buf + OFF_PAYLOAD, f.payload, f.payload_len);
    uint16_t pcrc = hap_crc16(buf + OFF_PAYLOAD, f.payload_len);
    buf[OFF_PAYLOAD + f.payload_len]     = pcrc & 0xFF;
    buf[OFF_PAYLOAD + f.payload_len + 1] = (pcrc >> 8) & 0xFF;
    return total;
}

HapDecodeResult hap_decode(const uint8_t* buf, size_t len, HapFrame& out) {
    if (len < OFF_HDR_CRC + 1) return HAP_DECODE_TRUNCATED;
    if (memcmp(buf + OFF_PREAMBLE, HAP_PREAMBLE, 3) != 0)
        return HAP_DECODE_BAD_MAGIC;
    if (buf[OFF_PREAMBLE + 3] != HAP_VERSION)
        return HAP_DECODE_BAD_VERSION;
    uint8_t hdr_crc_expected = hap_crc8(buf, OFF_HDR_CRC);
    if (hdr_crc_expected != buf[OFF_HDR_CRC])
        return HAP_DECODE_BAD_HDR_CRC;

    uint16_t plen = buf[OFF_LEN] | (static_cast<uint16_t>(buf[OFF_LEN + 1]) << 8);
    if (plen > HAP_MAX_PAYLOAD) return HAP_DECODE_OVERFLOW;
    size_t total = HAP_FRAME_OVERHEAD + plen;
    if (len < total) return HAP_DECODE_TRUNCATED;

    uint16_t pcrc_expected = hap_crc16(buf + OFF_PAYLOAD, plen);
    uint16_t pcrc_actual   = buf[OFF_PAYLOAD + plen]
                           | (static_cast<uint16_t>(buf[OFF_PAYLOAD + plen + 1]) << 8);
    if (pcrc_actual != pcrc_expected) return HAP_DECODE_CRC_ERROR;

    out.type        = static_cast<HapMsgType>(buf[OFF_TYPE]);
    out.flags       = buf[OFF_FLAGS];
    out.seq         = buf[OFF_SEQ]     | (static_cast<uint16_t>(buf[OFF_SEQ + 1])     << 8);
    out.ack_seq     = buf[OFF_ACK_SEQ] | (static_cast<uint16_t>(buf[OFF_ACK_SEQ + 1]) << 8);
    out.payload     = buf + OFF_PAYLOAD;
    out.payload_len = plen;
    return HAP_DECODE_OK;
}

HapDecodeResult hap_decode_stream(const uint8_t* buf, size_t len,
                                   HapFrame& out, size_t* consumed) {
    HapDecodeResult r = hap_decode(buf, len, out);
    if (r == HAP_DECODE_OK) {
        if (consumed) *consumed = HAP_FRAME_OVERHEAD + out.payload_len;
        return r;
    }
    if (r == HAP_DECODE_TRUNCATED) {
        if (consumed) *consumed = 0;
        return r;
    }
    for (size_t i = 1; i + 2 < len; i++) {
        if (buf[i]   == HAP_PREAMBLE[0] &&
            buf[i+1] == HAP_PREAMBLE[1] &&
            buf[i+2] == HAP_PREAMBLE[2]) {
            if (consumed) *consumed = i;
            // Caller should NOT log "CRC error at offset 0" — the truth
            // is "garbage skipped, fresh candidate preamble at offset i".
            // HAP_DECODE_RESYNC is documented as equivalent to a CRC
            // error for retry purposes (just clearer semantics).
            return HAP_DECODE_RESYNC;
        }
    }
    // No candidate preamble anywhere in the buffer (the forward scan above
    // now covers every full-preamble start position, tail included). The head
    // is undecodable and nothing downstream can resync, so discard the whole
    // buffer rather than re-presenting the same bad head next call. (A preamble
    // fragment straddling a read boundary is acceptably lost on this non-DMA
    // v3 path; the live P4<->S3 link uses the fixed-size two-stage DMA decoder.)
    if (consumed) *consumed = len;
    return r;
}

void hap_encode_stage1(const HapFrame& f, uint8_t out[HAP_STAGE1_LEN]) {
    memset(out, 0, HAP_STAGE1_LEN);
    memcpy(out + OFF_S1_PREAMBLE, HAP_PREAMBLE, 4);
    out[OFF_S1_TYPE]    = static_cast<uint8_t>(f.type);
    out[OFF_S1_FLAGS]   = f.flags;
    out[OFF_S1_SEQ]     = f.seq & 0xFF;
    out[OFF_S1_SEQ + 1] = (f.seq >> 8) & 0xFF;
    out[OFF_S1_ACK_SEQ]     = f.ack_seq & 0xFF;
    out[OFF_S1_ACK_SEQ + 1] = (f.ack_seq >> 8) & 0xFF;
    out[OFF_S1_LEN]     = f.payload_len & 0xFF;
    out[OFF_S1_LEN + 1] = (f.payload_len >> 8) & 0xFF;
    out[OFF_S1_RESERVED] = 0;
    uint16_t crc = hap_crc16(out, OFF_S1_HDR_CRC);
    out[OFF_S1_HDR_CRC]     = crc & 0xFF;
    out[OFF_S1_HDR_CRC + 1] = (crc >> 8) & 0xFF;
    out[HAP_STAGE1_LEN - 1] = 0;   // trailing pad byte (already zeroed by memset)
}

HapDecodeResult hap_decode_stage1(const uint8_t in[HAP_STAGE1_LEN], HapFrame& out) {
    if (memcmp(in + OFF_S1_PREAMBLE, HAP_PREAMBLE, 3) != 0)
        return HAP_DECODE_BAD_MAGIC;
    if (in[OFF_S1_PREAMBLE + 3] != HAP_VERSION)
        return HAP_DECODE_BAD_VERSION;
    uint16_t crc_expected = hap_crc16(in, OFF_S1_HDR_CRC);
    uint16_t crc_actual   = in[OFF_S1_HDR_CRC]
                          | (static_cast<uint16_t>(in[OFF_S1_HDR_CRC + 1]) << 8);
    if (crc_expected != crc_actual)
        return HAP_DECODE_BAD_HDR_CRC;
    uint16_t plen = in[OFF_S1_LEN] | (static_cast<uint16_t>(in[OFF_S1_LEN + 1]) << 8);
    if (plen > HAP_MAX_PAYLOAD)
        return HAP_DECODE_OVERFLOW;
    out.type        = static_cast<HapMsgType>(in[OFF_S1_TYPE]);
    out.flags       = in[OFF_S1_FLAGS];
    out.seq         = in[OFF_S1_SEQ]     | (static_cast<uint16_t>(in[OFF_S1_SEQ + 1])     << 8);
    out.ack_seq     = in[OFF_S1_ACK_SEQ] | (static_cast<uint16_t>(in[OFF_S1_ACK_SEQ + 1]) << 8);
    out.payload     = nullptr;        // stage 2 will populate
    out.payload_len = plen;
    return HAP_DECODE_OK;
}

size_t hap_encode_stage2(const HapFrame& f, uint8_t* out, size_t cap) {
    if (f.payload_len > 0 && !f.payload) return 0;   // Q16: see hap_encode
    size_t need = static_cast<size_t>(f.payload_len) + 2;
    if (need > cap) return 0;
    if (f.payload_len > 0 && f.payload)
        memcpy(out, f.payload, f.payload_len);
    uint16_t crc = hap_crc16(out, f.payload_len);
    out[f.payload_len]     = crc & 0xFF;
    out[f.payload_len + 1] = (crc >> 8) & 0xFF;
    return need;
}

bool hap_verify_stage2(const uint8_t* in, uint16_t plen) {
    uint16_t expected = hap_crc16(in, plen);
    uint16_t actual   = in[plen] | (static_cast<uint16_t>(in[plen + 1]) << 8);
    return expected == actual;
}
