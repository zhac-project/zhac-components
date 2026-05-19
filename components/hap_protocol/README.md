# hap_protocol — HAP Wire Format Specification

The HAP (Host Application Protocol) is the binary link between the ESP32-S3
WiFi/REST/MQTT gateway and the ESP32-P4 Zigbee/Lua engine over the on-board
SPI bus. This component is the *normative* description of every byte on the
wire and the only `host`-buildable codec for it; every other `hap_*`
component layers on top.

This document is the protocol specification. It is meant to be read by:

- new contributors writing a fresh peer (test harness, fuzzer, alternative
  transport binding such as UART or TCP);
- anyone authoring or debugging a message type;
- security review or red-team replay tooling.

It is intentionally not a tutorial. For walk-throughs use the integration
test suites in `test/` and the per-component READMEs of `hap_session`,
`hap_slave`, and `hap_master`.

---

## 1. Scope and role

```
hap_dispatch (P4)         api_handlers (S3)
        │                       │
hap_session   (reliability layer — sliding window, retry, ACK echo)
        │                       │
hap_master (S3) ──── SPI ──── hap_slave (P4)
        │                       │
hap_protocol (encode / decode / CRC)   ◀── this component
```

`hap_protocol` is pure logic — no FreeRTOS, no SPI driver — so it host-
builds and unit-tests cleanly. The CMakeLists.txt `REQUIRES zap_common`
purely for the SPDX/include macros; it has no ESP-IDF runtime
dependencies. Builds for `host`, `esp32s3`, and `esp32p4`.

## 2. Versions

| Version | First introduced | Status                  |
|---------|------------------|--------------------------|
| `0x01`  | initial          | dropped, no wire compat  |
| `0x02`  | added ACK_SEQ    | dropped                  |
| `0x03`  | widened LEN to 2 B, added HDR_CRC8 | dropped       |
| `0x04`  | added two-stage SPI DMA form (HDR_CRC16) | **current** |

The version is encoded as the fourth preamble byte (`HAP_PREAMBLE[3]`). It
is enforced both at compile time (`static_assert(HAP_PREAMBLE[3] ==
HAP_VERSION)` in `hap_protocol.h`) and at decode time
(`HAP_DECODE_BAD_VERSION`). Bumping one without the other is treated as a
build error.

Peers MUST refuse frames whose version does not match exactly. There is no
backward compatibility: the link runs whatever both sides were flashed
with, and a SYNC version-string check at boot logs a warning when the SPA
firmware bundle and P4 firmware disagree.

## 3. Preamble and constants

```c
constexpr uint8_t  HAP_PREAMBLE[4]    = {0xAA, 0x55, 0xFE, 0x04};
constexpr uint8_t  HAP_VERSION        = 0x04;
constexpr size_t   HAP_FRAME_OVERHEAD = 15;     // 13 hdr + 2 payload CRC16
constexpr size_t   HAP_MAX_PAYLOAD    = 4096;
constexpr size_t   HAP_MAX_FRAME_SIZE = HAP_FRAME_OVERHEAD + HAP_MAX_PAYLOAD;
constexpr size_t   HAP_STAGE1_LEN     = 16;     // v4 two-stage form
constexpr size_t   HAP_DMA_ALIGN      = 64;     // SPI DMA alignment
constexpr size_t   HAP_STAGE1_CLOCK_LEN = HAP_DMA_ALIGN;
```

`0xAA55` is the canonical sync marker; `0xFE` is a sentinel chosen because
it is rare in ASCII / JSON payloads, helping `hap_decode_stream` resync
inside a corrupted buffer. The fourth byte is the version, and is part of
the magic check on the receive side.

## 4. Frame layouts

There are two on-the-wire framings; both are decoded by this component.

### 4.1. Single-frame (non-DMA) layout

Used by host tests, the in-memory decoder paths, and any transport that
ships a contiguous byte stream without SPI's two-phase clocking
constraint.

```
Offset  Field         Size  Notes
─────────────────────────────────────────────────────────────────
0..3    PREAMBLE      4 B   0xAA 0x55 0xFE 0x04
4       TYPE          1 B   HapMsgType (see §6)
5       FLAGS         1 B   bitmap (see §5)
6..7    SEQ           2 B   LE  — sender's sequence; 0 reserved
8..9    ACK_SEQ       2 B   LE  — 0 = no correlation, else echoes req SEQ
10..11  LEN           2 B   LE  — payload length, 0..HAP_MAX_PAYLOAD
12      HDR_CRC8      1 B   CRC-8/CCITT (poly 0x07, init 0x00) over bytes 0..11
13..    PAYLOAD       LEN B JSON or binary; defined per message type
last 2  PAYLOAD_CRC16 2 B   CRC-CCITT (poly 0x1021, init 0xFFFF) over payload only
```

Total wire size = `HAP_FRAME_OVERHEAD + LEN` = `15 + LEN`.

### 4.2. Two-stage SPI DMA form

ESP-IDF SPI slave DMA on S3 / P4 requires both the buffer address AND the
transaction length to be aligned to 64 bytes; this prevents wrapping an
arbitrary single-frame payload directly into a DMA descriptor. The v4
solution splits each frame into two SPI transactions:

- **Stage 1** clocks a fixed 16-byte header padded out to 64 bytes
  (`HAP_STAGE1_CLOCK_LEN`). The trailing 48 bytes are zero and ignored.
  Stage-1 layout:

  ```
  Offset  Field        Size  Notes
  ─────────────────────────────────────────────────────────────
  0..3    PREAMBLE     4 B   same as 4.1
  4       TYPE         1 B
  5       FLAGS        1 B
  6..7    SEQ          2 B   LE
  8..9    ACK_SEQ      2 B   LE
  10..11  LEN          2 B   LE — stage-2 payload length
  12      RESERVED     1 B   0x00
  13..14  HDR_CRC16    2 B   CRC-CCITT (poly 0x1021, init 0xFFFF) over bytes 0..12
  15      pad          1 B   0x00 (zeroed for DMA alignment)
  ```

- **Stage 2** clocks `LEN + 2` bytes — payload followed by the payload
  CRC16 — rounded up to the next 64-byte boundary with trailing zeros
  that the decoder ignores.

The two CRCs (header CRC16 in stage 1, payload CRC16 in stage 2) protect
the two transactions independently. A torn stage-1 with a bit-flipped LEN
is rejected by HDR_CRC16 before stage 2 is even issued, so the slave
never reads a wildly-wrong number of bytes off the DMA channel.

Encoder / decoder helpers:

```c
void              hap_encode_stage1(const HapFrame& f, uint8_t out[HAP_STAGE1_LEN]);
HapDecodeResult   hap_decode_stage1(const uint8_t in[HAP_STAGE1_LEN], HapFrame& out);

size_t hap_encode_stage2(const HapFrame& f, uint8_t* out, size_t cap);
bool   hap_verify_stage2(const uint8_t* in, uint16_t plen);
```

The stage-1 helpers fill / verify the 16-byte header only; the caller
must zero-pad to `HAP_STAGE1_CLOCK_LEN` before posting the DMA
descriptor. The stage-2 helpers operate on a contiguous
`LEN + 2`-byte buffer (payload + appended CRC16); the caller again is
responsible for the DMA pad.

## 5. Frame flags

```c
inline constexpr uint8_t HAP_FLAG_NEEDS_ACK = 0x01;   // sender expects ACK echo
inline constexpr uint8_t HAP_FLAG_NO_ACK    = 0x02;   // fire-and-forget
```

- `HAP_FLAG_NEEDS_ACK` (`0x01`) — `hap_session` registers the frame in
  the sliding window, retransmits up to `MAX_RETRIES` (5) on
  `ACK_TIMEOUT_MS` (1000 ms) ticks, and fires `on_link_dead` once the
  retry budget is exhausted. The receiver MUST emit a `HapMsgType::ACK`
  carrying the same SEQ in ACK_SEQ.
- `HAP_FLAG_NO_ACK` (`0x02`) — large frames (`BULK_STATE_UPDATE`,
  `DEVICE_LIST`, `OTA_CHUNK`, `METRICS_RSP`, …) where reliability is
  layered above HAP. The session window is bypassed; the receiver does
  not ACK and the sender does not retransmit. Application-level
  correlation runs through ACK_SEQ.
- `flags == 0x00` — used by control frames (notably `SYNC`) that must
  bypass the window during link bring-up.

`HAP_FLAG_NEEDS_ACK` and `HAP_FLAG_NO_ACK` are mutually exclusive and
MUST NOT be set together.

## 6. Message type registry

Type byte values are stable. New entries append to the table; existing
entries are never repurposed or renumbered. The full enumeration lives in
`include/hap_protocol.h` (`enum class HapMsgType : uint8_t`); the table
below mirrors that enum and groups it by sub-protocol for orientation.

### 6.1. Framing / link control (0x01..0x0F)

| Code | Name      | Direction | Payload                              |
|------|-----------|-----------|---------------------------------------|
| 0x01 | CMD       | either    | generic command (legacy / reserved)  |
| 0x02 | EVT       | either    | generic event (legacy / reserved)    |
| 0x03 | ACK       | either    | empty; SEQ=0, ACK_SEQ echoes request  |
| 0x04 | ERR       | either    | `{code, msg}`                         |
| 0x05 | SYNC      | either    | `HapSyncInfo` JSON (boot handshake)  |
| 0x06 | STREAM    | either    | reserved for streaming payloads      |

### 6.2. Device pool (0x10..0x1F)

| Code | Name             | Direction | Payload                                   |
|------|------------------|-----------|--------------------------------------------|
| 0x10 | GET_DEVICES      | S3→P4     | empty                                       |
| 0x11 | DEVICE_LIST      | P4→S3     | JSON `{devices:[...]}`; NO_ACK             |
| 0x12 | GET_DEVICE_BY_ID | S3→P4     | `{ieee:"0x..."}`                            |
| 0x13 | DEVICE_INFO      | P4→S3     | JSON device object                          |
| 0x14 | SET_ATTRIBUTE    | S3→P4     | `{ieee, ep, cluster, attr, value}`         |
| 0x15 | SET_ACK          | P4→S3     | `{ok:bool}`                                 |

### 6.3. Device lifecycle (0x20..0x2F)

| Code | Name                 | Direction | Payload                                    |
|------|----------------------|-----------|---------------------------------------------|
| 0x20 | DEVICE_EVENT         | P4→S3     | generic device event                       |
| 0x21 | DEVICE_JOIN          | P4→S3     | `{ieee, friendly_name, ...}`                |
| 0x22 | DEVICE_LEAVE         | P4→S3     | `{ieee}`                                    |
| 0x23 | ALERT                | P4→S3     | `HapAlert` (e.g. battery low)              |
| 0x24 | DEVICE_SET_NAME      | S3→P4     | `{ieee, friendly_name}`                     |
| 0x25 | PERMIT_JOIN          | S3→P4     | `{duration:N}` (0 closes)                  |
| 0x27 | BIND_REQ             | S3→P4     | `HapBindReq`                                |
| 0x28 | BIND_ACK             | P4→S3     | `{ok:bool}`                                 |
| 0x29 | DEVICE_DELETE        | S3→P4     | `{ieee, hard:bool}`                         |
| 0x2A | DEVICE_DELETE_ACK    | P4→S3     | `{ok:bool}`                                 |
| 0x2B | INTERVIEW_REQ        | S3→P4     | `{ieee}` (fire-and-forget)                  |
| 0x2C | DEVICE_OPTIONS_SET   | S3→P4     | `{ieee, occupancy_timeout, ...}`            |
| 0x2D | DEVICE_OPTIONS_SET_ACK | P4→S3   | `{ok:bool}`                                 |
| 0x2E | CONFIGURE_REQ        | S3→P4     | `{ieee}` — re-run configure without full interview |

### 6.4. Rules and Lua scripts (0x30..0x3F)

| Code | Name              | Direction | Payload                                   |
|------|-------------------|-----------|--------------------------------------------|
| 0x30 | RULE_CREATE       | S3→P4     | rule JSON                                   |
| 0x31 | RULE_DELETE       | S3→P4     | `{rule_id}`                                 |
| 0x32 | RULE_EXEC_RESULT  | P4→S3     | `{ok, err}` — shared by all rule mutators  |
| 0x33 | RULE_LIST_REQ     | S3→P4     | empty                                       |
| 0x34 | RULE_LIST_RSP     | P4→S3     | JSON list                                   |
| 0x35 | RULE_UPDATE       | S3→P4     | rule JSON                                   |
| 0x36 | SCRIPT_WRITE      | S3→P4     | `{name, src}`                               |
| 0x37 | SCRIPT_ACK        | P4→S3     | `{ok, err}` — shared by all script mutators |
| 0x38 | SCRIPT_DELETE     | S3→P4     | `{name}`                                    |
| 0x39 | SCRIPT_LIST_REQ   | S3→P4     | empty                                       |
| 0x3A | SCRIPT_LIST_RSP   | P4→S3     | JSON list                                   |
| 0x3B | SCRIPT_READ_REQ   | S3→P4     | `{name}`                                    |
| 0x3C | SCRIPT_READ_RSP   | P4→S3     | `{name, src}`                               |
| 0x3D | RULE_UPDATE_DSL   | S3→P4     | DSL source text                             |
| 0x3E | MQTT_MSG_IN       | S3→P4     | `{topic, payload}`                          |
| 0x3F | TIME_SYNC         | S3→P4     | `{epoch}`                                   |

### 6.5. OTA (0x40..0x4F)

| Code | Name                 | Direction | Payload                                    |
|------|----------------------|-----------|---------------------------------------------|
| 0x40 | OTA_CHUNK            | S3→P4     | binary blob (NO_ACK)                       |
| 0x41 | OTA_STATUS           | P4→S3     | `HapOtaStatus`                              |
| 0x42 | OTA_CHECKPOINT_REQ   | S3→P4     | empty                                       |
| 0x43 | OTA_CHECKPOINT_RSP   | P4→S3     | `{offset:u32}`                              |

### 6.6. Diagnostics / configuration / metrics (0x50..0x5F)

| Code | Name                  | Direction | Payload                                    |
|------|-----------------------|-----------|---------------------------------------------|
| 0x50 | HEARTBEAT             | both      | `HapHeartbeat` (cadence 5 s, NO_ACK)       |
| 0x51 | ZIGBEE_FACTORY_RESET  | S3→P4     | empty (fire-and-forget, reboots P4)        |
| 0x52 | DIAG_UNHANDLED_REQ    | S3→P4     | empty                                       |
| 0x53 | DIAG_UNHANDLED_RSP    | P4→S3     | JSON array                                  |
| 0x54 | ZIGBEE_CFG_SET        | S3→P4     | `{channel, net_key_hex}`                    |
| 0x55 | ZIGBEE_CFG_SET_ACK    | P4→S3     | `{ok, channel, net_key_set}`                |
| 0x56 | METRICS_REQ           | S3→P4     | empty                                       |
| 0x57 | METRICS_RSP           | P4→S3     | Prometheus text, truncated at HAP_MAX_PAYLOAD |
| 0x58 | SCRIPT_RUN_REQ        | S3→P4     | `{name}` — fire one Lua coroutine          |
| 0x59 | SCRIPT_CHECK_REQ      | S3→P4     | `{name, src}` (parse-only)                  |
| 0x5A | SCRIPT_CHECK_RSP      | P4→S3     | `{ok, err, line}`                           |

### 6.7. Bulk / external (0x60..0x80)

| Code | Name              | Direction | Payload                                    |
|------|-------------------|-----------|---------------------------------------------|
| 0x60 | BULK_STATE_UPDATE | P4→S3     | per-attr device update JSON (NO_ACK)       |
| 0x70 | MQTT_PUBLISH      | P4→S3     | `{topic, payload, qos, retain}`            |
| 0x71 | TG_SETTOKEN       | P4→S3     | bot token                                   |
| 0x72 | TG_SETCHAT        | P4→S3     | default chat id                             |
| 0x73 | TG_SEND           | P4→S3     | `{chat?, parse_mode?, text}`                |
| 0x80 | LOG_LINE          | P4→S3     | `{lvl, tag, msg}`                          |

## 7. Sequence-number lifecycle

`SEQ` is a 16-bit counter maintained per chip. Implementation:
`hap_session_next_seq()` returns and post-increments an internal counter
that wraps from `0xFFFF` to `0x0001`. `SEQ == 0` is reserved and never
emitted; the decoder treats it as a sentinel for "no correlation" on
ACK_SEQ but a frame whose SEQ is 0 in encode is a contract violation.

Each chip's SEQ is independent — the S3's SEQ and the P4's SEQ are two
disjoint streams. The receiver does not validate monotonicity (small
out-of-order is possible across retransmits); it only uses SEQ to detect
duplicates inside the sliding window, and ACK_SEQ to correlate
responses.

### 7.1. ACK_SEQ correlation

For request/response pairs (`hap_roundtrip` on the S3 side, `hap_send`
with an `ack_seq` argument on the P4 side), the responder uses
`hap_make_reply()` (see `hap_protocol.h`) to construct a frame whose
`ack_seq = request.seq`. The caller looks up its pending requests by the
response type; the per-type expected-seq slot is set at request time and
cleared on receive.

When the slot contents do not match the incoming `ack_seq`, the frame is
dropped as stale. This guards against the case where a previous timed-out
request's late reply would otherwise unblock the next call on the same
per-type semaphore.

Note: ACK_SEQ is distinct from `HAP_FLAG_NEEDS_ACK`. ACK_SEQ is
application-level correlation; `NEEDS_ACK` is transport-level reliability.
A frame may carry both, neither, or one without the other.

## 8. CRCs

Two distinct CRC algorithms are used, depending on the framing form.

### 8.1. `hap_crc16` — CRC-CCITT / XMODEM

```c
poly = 0x1021,  init = 0xFFFF,  no XOR-out,  MSB-first
```

Used for: payload CRC in both single-frame and two-stage forms, AND the
header CRC of stage-1 in the two-stage form. Reference implementation in
`hap_protocol.cpp`.

### 8.2. `hap_crc8` — CRC-8/CCITT

```c
poly = 0x07,  init = 0x00,  no XOR-out,  MSB-first
```

Used for: header CRC of the single-frame form only. Strong enough for a
12-byte header and one byte cheaper on the wire; payload CRC16 catches
anything the header CRC8 might miss.

Both CRCs are byte-aligned and computed over the same offsets specified
in §4.

## 9. Decode error codes

```c
enum HapDecodeResult {
    HAP_DECODE_OK          = 0,
    HAP_DECODE_BAD_MAGIC   = 1,   // PREAMBLE[0..2] mismatch
    HAP_DECODE_BAD_VERSION = 2,   // PREAMBLE[3] != HAP_VERSION
    HAP_DECODE_CRC_ERROR   = 3,   // payload CRC16 mismatch
    HAP_DECODE_TRUNCATED   = 4,   // not enough bytes — caller buffers more
    HAP_DECODE_OVERFLOW    = 5,   // LEN > HAP_MAX_PAYLOAD
    HAP_DECODE_BAD_HDR_CRC = 6,   // HDR_CRC mismatch (CRC8 or stage-1 CRC16)
    HAP_DECODE_RESYNC      = 7,   // hap_decode_stream only — see §10
};
```

## 10. Stream resync (`hap_decode_stream`)

For lossy transports (SPI signal-integrity events, premature pre-empt,
…) a single bad frame must not poison all subsequent frames. The
`hap_decode_stream` wrapper:

```c
HapDecodeResult hap_decode_stream(const uint8_t* buf, size_t len,
                                   HapFrame& out, size_t* consumed);
```

Behaviour:

- If decode succeeds at offset 0: returns `HAP_DECODE_OK` with `*consumed
  = frame_total_bytes`. Caller advances by that many bytes.
- If decode would need more bytes: returns `HAP_DECODE_TRUNCATED` with
  `*consumed = 0`. Caller buffers more bytes and retries.
- Otherwise the scanner walks forward looking for the next 3-byte
  PREAMBLE candidate. If found at offset `i`: returns `HAP_DECODE_RESYNC`
  with `*consumed = i`. Caller skips `i` bytes and calls again. If no
  candidate is found: returns the original decode error code with
  `*consumed = len - 3` (keeping the trailing 3 bytes in case a fresh
  preamble straddles the next buffer fill).

`HAP_DECODE_RESYNC` is documented semantically equivalent to
`HAP_DECODE_CRC_ERROR` for retry purposes — it exists to disambiguate
"garbage skipped, fresh candidate located" from "frame validated CRC
fail at offset 0", so logs and metrics do not misattribute the failure.

Callers using `hap_decode_with_counters` (`include/hap_protocol_decode.h`)
get an automatic `METRIC_HAP_RESYNC_BYTES` counter increment whenever
`*consumed > 0`.

## 11. SYNC and HEARTBEAT control flow

### 11.1. Boot SYNC handshake

1. S3 boots, brings up the SPI master, and enters its task_hap loop.
2. S3 sends `SYNC` (TYPE = 0x05, FLAGS = 0x00, payload =
   `hap_json_encode_sync_req`). The session-layer window is bypassed so
   the frame ships even before the peer is known to be alive.
3. P4 receives the SYNC and responds with another `SYNC` whose payload
   has `is_ack = true` and includes the P4's firmware version and the
   current device-pool count.
4. On receipt the S3 latches `s_synced = true` and unblocks any pending
   REST calls that were waiting on the link.
5. If a SYNC is not answered within 2 seconds, the S3 re-issues it. This
   continues indefinitely; there is no "link dead" state at SYNC level
   — only the retransmit layer (§5) raises that.

The SYNC payload version string is the firmware version — *not* the
HAP_VERSION byte. A mismatch logs a warning but does not block the link;
the wire-level HAP version is enforced separately by HAP_DECODE_BAD_VERSION.

### 11.2. HEARTBEAT cadence

Every 5 seconds (`HAP_HEARTBEAT_INTERVAL_MS`), each chip emits a
`HEARTBEAT` (TYPE = 0x50, FLAGS = `HAP_FLAG_NO_ACK`) carrying:

- uptime in seconds,
- memory snapshot (free / min-free / largest-block, internal + PSRAM),
- per-core CPU percent,
- `proto_mask` describing which sub-protocols are healthy,
- the chip's authoritative device count (`pool_count_active()` on P4).

If a chip does not receive three consecutive heartbeats from its peer
(15 s by default), it raises a `p4_unresponsive` / `s3_unresponsive`
WebSocket / MQTT alert. Receipt of a fresh heartbeat clears the flag.
HEARTBEAT is the single writer of the cached peer-device-count atomic on
the S3 side (`s_p4_device_count`); other paths used to also update it
but were a source of races (see CHANGELOG F-10).

## 12. Encoder / decoder API

The two entry points and four helpers in `include/hap_protocol.h` are
all the on-the-wire surface this component exposes:

```c
// Single-frame form
size_t          hap_encode(const HapFrame& frame, uint8_t* buf, size_t buf_size);
HapDecodeResult hap_decode(const uint8_t* buf, size_t len, HapFrame& out);
HapDecodeResult hap_decode_stream(const uint8_t* buf, size_t len,
                                   HapFrame& out, size_t* consumed);

// Two-stage SPI DMA form
void            hap_encode_stage1(const HapFrame& f, uint8_t out[HAP_STAGE1_LEN]);
HapDecodeResult hap_decode_stage1(const uint8_t in[HAP_STAGE1_LEN], HapFrame& out);
size_t          hap_encode_stage2(const HapFrame& f, uint8_t* out, size_t cap);
bool            hap_verify_stage2(const uint8_t* in, uint16_t plen);

// CRC primitives (callable directly for tooling / fuzzing)
uint16_t hap_crc16(const uint8_t* data, size_t len);
uint8_t  hap_crc8 (const uint8_t* data, size_t len);

// Build a response frame that correlates to `req` via ACK_SEQ
HapFrame hap_make_reply(const HapFrame& req, HapMsgType rsp_type, uint8_t flags = 0);
```

`HapFrame.payload` is a non-owning `const uint8_t*` — it must remain
valid for the duration of the encode / send call. The session layer
copies the payload before queuing so the caller may reuse its buffer
immediately after `hap_session_send` returns.

## 13. Forward compatibility

- New message-type codes append to the registry. Decoders MUST NOT panic
  on an unknown TYPE — current implementations log a warning and drop
  the frame.
- Adding a field to an existing JSON payload is a wire-compatible change.
  Removing or renaming a field is not; bump HAP_VERSION instead.
- Changing any CRC polynomial, the LEN field width, or the preamble bytes
  is a hard break; bump HAP_VERSION.

## 14. References

- `include/hap_protocol.h` — constants, enums, prototypes.
- `hap_protocol.cpp` — reference encode / decode implementation.
- `include/hap_protocol_decode.h` — counter-instrumented decode wrapper.
- `test/test_hap_frame.cpp` — unit-test corpus, including the
  v3-LEN-bit-flip and resync regression tests.
- `../hap_session/` — sliding-window reliability layer.
- `../hap_master/`, `../hap_slave/` — SPI transport bindings.
- `../hap_json/` — encoders / decoders for each TYPE's payload schema.
