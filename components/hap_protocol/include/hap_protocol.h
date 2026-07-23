// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>

static constexpr uint8_t  HAP_PREAMBLE[4]    = {0xAA, 0x55, 0xFE, 0x04};
static constexpr uint8_t  HAP_VERSION        = 0x04;
static constexpr size_t   HAP_FRAME_OVERHEAD = 15;   // 13 hdr+HDR_CRC16 + 2 payload CRC16
static constexpr size_t   HAP_MAX_PAYLOAD    = 4096;
static constexpr size_t   HAP_MAX_FRAME_SIZE = HAP_FRAME_OVERHEAD + HAP_MAX_PAYLOAD;

// Catch silent skew between the PREAMBLE byte 3 and HAP_VERSION: bumping
// one without the other is the textbook way to brick wire-compat between
// chips that were flashed at different commits. See README.md.
static_assert(HAP_PREAMBLE[3] == HAP_VERSION,
              "PREAMBLE[3] must equal HAP_VERSION — bump both together");

// Wire layout (v4 — single-frame / non-DMA form). The v4 two-stage SPI DMA
// form uses a different stage-1 header (16 B, CRC16 at [13..14]); see
// README.md and the OFF_S1_* / HAP_STAGE1_LEN constants below.
// [0..3]   PREAMBLE  (0xAA 0x55 0xFE 0x04 — magic16 + sentinel + version)
// [4]      TYPE
// [5]      FLAGS
// [6..7]   SEQ       (LE)
// [8..9]   ACK_SEQ   (LE)
// [10..11] LEN       (LE)
// [12]     HDR_CRC8  (CCITT, poly 0x07, init 0x00, over bytes 0..11)
// [13..13+LEN-1] PAYLOAD
// [last 2] PAYLOAD_CRC16 (CCITT, poly 0x1021, init 0xFFFF, over payload only)
//
// ACK_SEQ = 0 means "no correlation" (legacy/unsolicited frames). When a
// frame is a response to a specific request, ACK_SEQ echoes the request's
// SEQ so the caller on the other side can match response → request without
// relying on type-based semaphores (which can be satisfied by stale late
// replies after a timeout).
static constexpr size_t OFF_PREAMBLE = 0;
static constexpr size_t OFF_TYPE     = 4;
static constexpr size_t OFF_FLAGS    = 5;
static constexpr size_t OFF_SEQ      = 6;
static constexpr size_t OFF_ACK_SEQ  = 8;
static constexpr size_t OFF_LEN      = 10;
static constexpr size_t OFF_HDR_CRC  = 12;
static constexpr size_t OFF_PAYLOAD  = 13;

// v4 two-stage transport layout.
static constexpr size_t HAP_STAGE1_LEN       = 16;
// ESP-IDF SPI slave DMA on S3/P4 requires both buffer address AND
// transaction length to be aligned to 64 bytes. Clock the stage-1 header
// in a 64-byte slot (last 48 bytes are zero and ignored by the decoder),
// and round stage-2 length up to 64 likewise.
static constexpr size_t HAP_DMA_ALIGN        = 64;
static constexpr size_t HAP_STAGE1_CLOCK_LEN = HAP_DMA_ALIGN;
static constexpr size_t OFF_S1_PREAMBLE = 0;
static constexpr size_t OFF_S1_TYPE     = 4;
static constexpr size_t OFF_S1_FLAGS    = 5;
static constexpr size_t OFF_S1_SEQ      = 6;
static constexpr size_t OFF_S1_ACK_SEQ  = 8;
static constexpr size_t OFF_S1_LEN      = 10;
static constexpr size_t OFF_S1_RESERVED = 12;
static constexpr size_t OFF_S1_HDR_CRC  = 13;

enum class HapMsgType : uint8_t {
    CMD    = 0x01, EVT    = 0x02, ACK    = 0x03,
    ERR    = 0x04, SYNC   = 0x05, STREAM = 0x06,
    GET_DEVICES       = 0x10, DEVICE_LIST       = 0x11,
    GET_DEVICE_BY_ID  = 0x12, DEVICE_INFO       = 0x13,
    SET_ATTRIBUTE     = 0x14, SET_ACK           = 0x15,
    GROUP_MEMBER_QUERY = 0x16,  // S3→P4: {"ieee","ep"} — read device ZCL group membership
    GROUP_MEMBER_LIST  = 0x17,  // P4→S3: {"ok":bool,"gids":[...]} (Get Group Membership readback)
    DEVICE_EVENT      = 0x20, DEVICE_JOIN       = 0x21,
    DEVICE_LEAVE      = 0x22, ALERT             = 0x23,
    DEVICE_SET_NAME   = 0x24,
    PERMIT_JOIN       = 0x25,  // S3→P4: {"duration":N}  (0=close)
    BIND_REQ          = 0x27,  // S3→P4
    BIND_ACK          = 0x28,  // P4→S3
    DEVICE_DELETE     = 0x29,  // S3→P4: {"ieee":"0x..."}
    DEVICE_DELETE_ACK = 0x2A,  // P4→S3: {"ok":true/false}
    INTERVIEW_REQ     = 0x2B,  // S3→P4: {"ieee":"0x..."} fire-and-forget
    DEVICE_OPTIONS_SET     = 0x2C,  // S3→P4: {"ieee":"0x...","occupancy_timeout":N}
    DEVICE_OPTIONS_SET_ACK = 0x2D,  // P4→S3: {"ok":true/false}
    CONFIGURE_REQ          = 0x2E,  // S3→P4: {"ieee":"0x..."} fire-and-forget;
                                    // P4 re-runs run_configure (bindings +
                                    // reports + config_steps) without
                                    // redoing the full ZNP interview. Use
                                    // after a firmware update adds new
                                    // wiring (e.g. read-on-join attrs) to
                                    // an existing paired device's def.
                                    // Payload shape is identical to
                                    // INTERVIEW_REQ — same hap_json
                                    // encoder/decoder pair serves both.
    RULE_CREATE       = 0x30, RULE_DELETE       = 0x31,
    RULE_EXEC_RESULT  = 0x32, RULE_LIST_REQ    = 0x33,
    RULE_LIST_RSP     = 0x34, RULE_UPDATE       = 0x35,
    SCRIPT_WRITE      = 0x36, SCRIPT_ACK        = 0x37,
    SCRIPT_DELETE     = 0x38, SCRIPT_LIST_REQ   = 0x39,
    SCRIPT_LIST_RSP   = 0x3A, SCRIPT_READ_REQ   = 0x3B,
    SCRIPT_READ_RSP   = 0x3C,
    RULE_UPDATE_DSL   = 0x3D,
    MQTT_MSG_IN       = 0x3E,
    TIME_SYNC         = 0x3F,
    OTA_CHUNK              = 0x40, OTA_STATUS             = 0x41,
    OTA_CHECKPOINT_REQ     = 0x42, OTA_CHECKPOINT_RSP     = 0x43,
    HEARTBEAT              = 0x50,
    ZIGBEE_FACTORY_RESET   = 0x51,  // S3→P4: fire-and-forget, erases all Zigbee data + reboots
    DIAG_UNHANDLED_REQ     = 0x52,  // S3→P4: request unhandled-frame ring snapshot
    DIAG_UNHANDLED_RSP     = 0x53,  // P4→S3: JSON array of ring entries
    ZIGBEE_CFG_SET         = 0x54,  // S3→P4: {channel:N, net_key_hex:"…"}
    ZIGBEE_CFG_SET_ACK     = 0x55,  // P4→S3: {ok:true/false, channel:N,
                                    // net_key_set:bool}
    METRICS_REQ            = 0x56,  // S3→P4: empty payload, asks for a
                                    // Prometheus text snapshot
    METRICS_RSP            = 0x57,  // P4→S3: raw Prometheus text body
                                    // (prefix `zhac_p4_`); bounded by
                                    // HAP_MAX_PAYLOAD, truncated on
                                    // overflow
    SCRIPT_RUN_REQ         = 0x58,  // S3→P4: {"name":"<script>"}.
                                    // P4 enqueues one Lua coroutine
                                    // invocation and replies with
                                    // SCRIPT_ACK (ok / err).
    SCRIPT_CHECK_REQ       = 0x59,  // S3→P4: {"name":"…","src":"…"}.
                                    // Parse-only (luaL_loadbufferx mode
                                    // "t"); no execute. Replies with
                                    // SCRIPT_CHECK_RSP carrying the
                                    // compiler error and 1-based line.
    SCRIPT_CHECK_RSP       = 0x5A,  // P4→S3: {"ok":bool,"err":"…","line":N}.
    BULK_STATE_UPDATE = 0x60,
    MQTT_PUBLISH      = 0x70,
    TG_SETTOKEN       = 0x71,  // P4→S3: bot token string
    TG_SETCHAT        = 0x72,  // P4→S3: default chat id string
    TG_SEND           = 0x73,  // P4→S3: send msg (optional chat override + parse_mode + text)
    LOG_LINE          = 0x80,
};

struct HapFrame {
    HapMsgType     type;
    uint16_t       seq;
    uint16_t       ack_seq;      // 0 = no correlation; else echoes request seq
    uint8_t        flags;
    const uint8_t* payload;      // points into caller buffer — not owned
    uint16_t       payload_len;
};

enum HapDecodeResult {
    HAP_DECODE_OK          = 0,
    HAP_DECODE_BAD_MAGIC   = 1,
    HAP_DECODE_BAD_VERSION = 2,
    HAP_DECODE_CRC_ERROR   = 3,
    HAP_DECODE_TRUNCATED   = 4,
    HAP_DECODE_OVERFLOW    = 5,
    HAP_DECODE_BAD_HDR_CRC = 6,
    // hap_decode_stream-only: the current head was bad but a candidate
    // preamble was located further into the buffer. `*consumed` is the
    // byte count the caller must skip; the caller should treat this the
    // same as HAP_DECODE_CRC_ERROR for retry purposes but does NOT need
    // to log "CRC error at offset 0" — that diagnostic would be wrong.
    HAP_DECODE_RESYNC      = 7,
};

// ── v3 single-frame framing (CRC8 header + CRC16 payload) ────────────────
// Retained for the host test suite and any non-DMA / single-shot transport.
// The live P4<->S3 SPI link does NOT use these — it uses the v4 two-stage
// DMA helpers below (hap_encode_stage1/hap_decode_stage1/hap_encode_stage2/
// hap_verify_stage2). Keep both in sync if the wire header layout changes.

// Encode frame into buf. Returns total bytes written, 0 on overflow.
size_t hap_encode(const HapFrame& frame, uint8_t* buf, size_t buf_size);

// Decode frame from buf of length len. Fills out on success.
HapDecodeResult hap_decode(const uint8_t* buf, size_t len, HapFrame& out);

// Streaming decode: if the buffer doesn't start at a MAGIC, scan forward
// looking for the next 0xAA55 and try decode there. `*consumed` (if non-null)
// is set to the number of bytes that should be skipped/consumed by the
// caller — equals 0 on TRUNCATED (more bytes needed), equals the full
// decoded frame length on OK, or equals the number of bytes skipped past
// before a fresh candidate magic on decode failure.
//
// Use this when reading from a lossy stream (SPI signal integrity events)
// where a single bad frame must not poison all subsequent frames.
HapDecodeResult hap_decode_stream(const uint8_t* buf, size_t len,
                                   HapFrame& out, size_t* consumed);

// v4 stage-1 helpers. Stage 1 carries enough metadata for both peers to size
// stage 2; the payload + payload CRC are exchanged in stage 2.
void              hap_encode_stage1(const HapFrame& f, uint8_t out[HAP_STAGE1_LEN]);
HapDecodeResult   hap_decode_stage1(const uint8_t in[HAP_STAGE1_LEN], HapFrame& out);

// v4 stage-2 helpers. Caller is responsible for clocking the right number
// of bytes; helpers only encode/verify the local frame fragment.
size_t hap_encode_stage2(const HapFrame& f, uint8_t* out, size_t cap);
bool   hap_verify_stage2(const uint8_t* in, uint16_t plen);

// CRC-CCITT (poly 0x1021, init 0xFFFF)
uint16_t hap_crc16(const uint8_t* data, size_t len);

// CRC-8/CCITT (poly 0x07, init 0x00) — used for v3 header integrity.
uint8_t hap_crc8(const uint8_t* data, size_t len);

// Build a response frame that correlates to `req` by echoing its seq as
// ack_seq. Caller must still populate payload / payload_len and assign a
// fresh `seq` (via hap_session_next_seq() or equivalent) before sending.
//
// Example:
//   HapFrame rsp = hap_make_reply(req, HapMsgType::SET_ACK, HAP_FLAG_NO_ACK);
//   rsp.seq         = hap_session_next_seq();
//   rsp.payload     = buf;
//   rsp.payload_len = len;
//   hap_slave_send(rsp);
inline HapFrame hap_make_reply(const HapFrame& req, HapMsgType rsp_type,
                                uint8_t flags = 0) {
    HapFrame r{};
    r.type    = rsp_type;
    r.ack_seq = req.seq;       // correlate
    r.flags   = flags;
    return r;
}
