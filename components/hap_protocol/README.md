# hap_protocol — HAP Binary Framing

The wire format and codec for every byte exchanged between ESP32-S3 (WiFi gateway)
and ESP32-P4 (Zigbee coordinator) over the SPI link. This component is pure logic —
no FreeRTOS, no SPI driver — so it host-builds and unit-tests cleanly. Every other
HAP-* component layers on top of it.

## Where it sits

```
hap_dispatch (P4)         api_handlers (S3)
        │                       │
hap_session (S3 only — sliding window, retry, ACK)
        │                       │
hap_master (S3) ──── SPI ──── hap_slave (P4)
        │                       │
hap_protocol (encode / decode / CRC) ◀── this component
```

Both chips link `hap_protocol`. `hap_session` adds reliability (S3 only); the SPI
drivers move bytes; `hap_json` marshals payloads.

## Dependencies (CMakeLists.txt)

`REQUIRES zap_common` — only for the shared `static_assert`-able types. No
ESP-IDF runtime dependencies. Builds for `host`, `esp32s3`, and `esp32p4`.

## Wire format (v2)

```
Offset  Field      Size  Notes
─────────────────────────────────────────────────────────────────
0..1    MAGIC      2 B   0xAA55, little-endian
2       VERSION    1 B   0x02 (v1 had no ACK_SEQ; v2 added it)
3       TYPE       1 B   HapMsgType
4..5    SEQ        2 B   Sender's sequence (1..65535, 0 reserved)
6       FLAGS      1 B   HAP_FLAG_NEEDS_ACK | HAP_FLAG_NO_ACK
7..8    ACK_SEQ    2 B   0 = no correlation; else echoes request SEQ
9..10   LEN        2 B   payload length (0..4096)
11..    PAYLOAD    LEN B JSON or binary
last 2  CRC16      2 B   CRC-CCITT (poly 0x1021, init 0xFFFF) over [0..LEN+11)
```

Constants (`include/hap_protocol.h`):

| Constant              | Value    | Meaning                                 |
|-----------------------|----------|-----------------------------------------|
| `HAP_MAGIC`           | `0xAA55` | Frame sync marker                       |
| `HAP_VERSION`         | `0x02`   | Bumped from v1 when ACK_SEQ was added   |
| `HAP_FRAME_OVERHEAD`  | `14`     | header(12) + CRC16(2)                   |
| `HAP_MAX_PAYLOAD`     | `4096`   | Raw payload cap                         |
| `HAP_MAX_FRAME_SIZE`  | `4110`   | overhead + max payload                  |

`OFF_MAGIC=0`, `OFF_VER=2`, `OFF_TYPE=3`, `OFF_SEQ=4`, `OFF_FLAGS=6`,
`OFF_ACK_SEQ=7`, `OFF_LEN=9`, `OFF_PAYLOAD=11`.

### ACK_SEQ semantics

`ACK_SEQ = 0` means "this frame is unsolicited or stand-alone" (heartbeats,
device-join notifications, ACK frames themselves where SEQ already carries the
correlation). When the sender wants a correlated reply, it puts the request's
SEQ into the response's ACK_SEQ. `hap_make_reply()` (header inline) is the
canonical way to build a correlated response.

## Public API (`include/hap_protocol.h`)

```cpp
// Frame value type — payload is a borrowed pointer into caller buffer
struct HapFrame {
    HapMsgType     type;
    uint16_t       seq;
    uint16_t       ack_seq;     // 0 = uncorrelated, else echoes request seq
    uint8_t        flags;
    const uint8_t* payload;     // NOT owned by HapFrame
    uint16_t       payload_len;
};

// Encode `frame` into `buf` (caller-supplied). Returns total bytes written
// (header + payload + CRC), or 0 on overflow. Used by hap_master_send and
// hap_slave_send before clocking onto the SPI bus.
size_t hap_encode(const HapFrame& frame, uint8_t* buf, size_t buf_size);

// Decode exactly one frame starting at buf[0]. Caller must ensure buf points
// to a frame boundary. Returns one of HAP_DECODE_OK, _BAD_MAGIC,
// _BAD_VERSION, _CRC_ERROR, _TRUNCATED, _OVERFLOW.
HapDecodeResult hap_decode(const uint8_t* buf, size_t len, HapFrame& out);

// Streaming decode for lossy SPI streams. If buf doesn't start at MAGIC,
// scans forward for the next 0xAA55 and tries to decode there. *consumed
// reports how many bytes the caller should advance by:
//   - 0 on TRUNCATED (need more data)
//   - frame length on OK
//   - bytes-skipped on a recoverable error (resume from buf+*consumed)
// Use in hap_slave_task / hap_master receive paths so a single signal-
// integrity event doesn't poison the rest of the stream.
HapDecodeResult hap_decode_stream(const uint8_t* buf, size_t len,
                                   HapFrame& out, size_t* consumed);

// CRC-CCITT (poly 0x1021, init 0xFFFF). Hot path; ~95 LOC of pure C++.
uint16_t hap_crc16(const uint8_t* data, size_t len);

// Inline helper — builds a response frame correlated to `req` by echoing
// req.seq into r.ack_seq. Caller must still set seq, payload, payload_len.
inline HapFrame hap_make_reply(const HapFrame& req, HapMsgType rsp_type,
                                uint8_t flags = 0);
```

### Decode result codes

| Enum                    | When                                                |
|-------------------------|-----------------------------------------------------|
| `HAP_DECODE_OK` (0)     | Valid frame, `out` populated.                       |
| `HAP_DECODE_BAD_MAGIC`  | First two bytes not `0xAA55`.                       |
| `HAP_DECODE_BAD_VERSION`| Version byte ≠ `HAP_VERSION`. Logged once per side. |
| `HAP_DECODE_CRC_ERROR`  | CRC mismatch. Usually signal integrity.             |
| `HAP_DECODE_TRUNCATED`  | Buffer shorter than `HAP_FRAME_OVERHEAD + LEN`.     |
| `HAP_DECODE_OVERFLOW`   | LEN field exceeds `HAP_MAX_PAYLOAD`.                |

## Flags

```cpp
HAP_FLAG_NEEDS_ACK = 0x01   // sender expects ACK echo via hap_session
HAP_FLAG_NO_ACK    = 0x02   // explicitly fire-and-forget (skips window)
```

Defined in `hap_session.h` (not `hap_protocol.h`) because they only matter to
the session layer. Frames with `NO_ACK` bypass the sliding window even if
their type would otherwise queue.

## Message types (`HapMsgType`, full list)

All codes are hex. Direction shows the dominant flow; some types are bidirectional.

| Code | Name | Dir | Purpose |
|------|------|-----|---------|
| 0x01 | `CMD` | both | Generic command envelope |
| 0x02 | `EVT` | P4→S3 | Generic event |
| 0x03 | `ACK` | both | Session-layer acknowledgment |
| 0x04 | `ERR` | both | Error response |
| 0x05 | `SYNC` | both | Session negotiation |
| 0x06 | `STREAM` | reserved | (unused today) |
| 0x10 | `GET_DEVICES` | S3→P4 | Request full device list |
| 0x11 | `DEVICE_LIST` | P4→S3 | All paired devices, paginated |
| 0x12 | `GET_DEVICE_BY_ID` | S3→P4 | Single device by IEEE |
| 0x13 | `DEVICE_INFO` | P4→S3 | Device exposes/state snapshot |
| 0x14 | `SET_ATTRIBUTE` | S3→P4 | Write a ZCL attribute |
| 0x15 | `SET_ACK` | P4→S3 | Write ack/err (correlated by ACK_SEQ) |
| 0x20 | `DEVICE_EVENT` | P4→S3 | Generic per-device event |
| 0x21 | `DEVICE_JOIN` | P4→S3 | Pairing notification |
| 0x22 | `DEVICE_LEAVE` | P4→S3 | Unpairing notification |
| 0x23 | `ALERT` | P4→S3 | Battery low, crash, etc. |
| 0x24 | `DEVICE_SET_NAME` | S3→P4 | Friendly-name update |
| 0x25 | `PERMIT_JOIN` | S3→P4 | `{duration:N}` (0 closes net) |
| 0x27 | `BIND_REQ` / 0x28 `BIND_ACK` | both | Zigbee bind table change |
| 0x29 | `DEVICE_DELETE` / 0x2A `..._ACK` | both | Force-remove device |
| 0x2B | `INTERVIEW_REQ` | S3→P4 | Re-interview (fire-and-forget) |
| 0x2C | `DEVICE_OPTIONS_SET` / 0x2D `..._ACK` | both | Per-device tuning (e.g. occupancy_timeout) |
| 0x30..0x35 | `RULE_*` | both | Rule create / delete / update / list / exec result |
| 0x36..0x3C | `SCRIPT_*` | both | Lua script write / delete / list / read |
| 0x3D | `RULE_UPDATE_DSL` | S3→P4 | Replace rule source via DSL |
| 0x3E | `MQTT_MSG_IN` | S3→P4 | Inbound MQTT forwarded to scripts |
| 0x3F | `TIME_SYNC` | S3→P4 | Push Unix ts (re-sent hourly) |
| 0x40..0x43 | `OTA_*` | both | OTA chunks, status, checkpoint |
| 0x50 | `HEARTBEAT` | both | Uptime / heap / cpu / proto_mask |
| 0x51 | `ZIGBEE_FACTORY_RESET` | S3→P4 | Erase Zigbee NVS + reboot (FAF) |
| 0x52 | `DIAG_UNHANDLED_REQ` / 0x53 `..._RSP` | both | Unhandled-frame ring snapshot |
| 0x54 | `ZIGBEE_CFG_SET` / 0x55 `..._ACK` | both | Channel + net key change |
| 0x56 | `METRICS_REQ` / 0x57 `..._RSP` | both | Prometheus text snapshot from P4 |
| 0x58 | `SCRIPT_RUN_REQ` | S3→P4 | One-shot Lua coroutine invocation |
| 0x59 | `SCRIPT_CHECK_REQ` / 0x5A `..._RSP` | both | Parse-only Lua syntax check |
| 0x60 | `BULK_STATE_UPDATE` | P4→S3 | Per-attr `device_update` JSON emitted live from `hap_dispatch::on_zcl_attr_for_hap`. (The batched `flush_bulk` / `bulk_push` path was retired 2026-04-25 — CC-F6 in `docs/FINDINGS.md`.) |
| 0x70 | `MQTT_PUBLISH` | both | P4→S3 publish forward; also S3 self-publish |
| 0x80 | `LOG_LINE` | both | Forward `ESP_LOGx` lines (sink configurable) |

The full enum lives at `include/hap_protocol.h:31-86`. JSON shape per type is
documented in `components/hap_json/README.md`.

## Threading and concurrency

`hap_protocol` is purely functional — every call works on caller-supplied
buffers. There is no global state, no mutex, and no allocation. Safe to call
from any task or ISR (avoid the encode/decode hot path inside ISRs only because
they run for ~20 µs per 4 KB frame).

## Error and failure modes

| Symptom                                           | Source                                          |
|---------------------------------------------------|-------------------------------------------------|
| `RX bad magic 0x..` (hap_master / hap_slave)      | Stream lost framing; `hap_decode_stream` re-syncs. |
| `RX CRC mismatch (signal integrity?)`             | One bad frame; counter advances, frame dropped. |
| `RX HAP_VERSION mismatch (got 0x.., expected 0x..)` | Mixed-firmware S3 / P4. Reflash both. |
| `recv truncated frame`                            | DMA buffer cut a frame; next loop re-reads.     |

No frames are silently dropped from `hap_protocol` itself — failures bubble up
to the master/slave's RX log line.

## Integration example

Encoding a device-join notification on the P4 side:

```cpp
#include "hap_protocol.h"
#include "hap_json.h"
#include "hap_slave.h"
#include "hap_session.h"

uint8_t  payload[64];
uint16_t plen = 0;
hap_json_encode_device_join(payload, sizeof(payload), &plen, ieee, nwk);

HapFrame f{};
f.type        = HapMsgType::DEVICE_JOIN;
f.seq         = hap_session_next_seq();
f.flags       = 0;                       // unsolicited; no ACK needed
f.payload     = payload;
f.payload_len = plen;
hap_slave_send(f);                       // hap_master_send on the S3 side
```

Decoding from a stream-oriented receiver:

```cpp
size_t consumed = 0;
HapFrame out;
HapDecodeResult r = hap_decode_stream(buf, buf_len, out, &consumed);
if (r == HAP_DECODE_OK) {
    handle(out);
}
buf     += consumed;
buf_len -= consumed;
```

## Cross-references

- `docs/FINDINGS.md` — architecture audit + dual-chip rationale
- `components/hap_session/README.md` — sliding-window reliability above this layer
- `components/hap_master/README.md`, `components/hap_slave/README.md` — SPI transport
- `components/hap_json/README.md` — JSON payload shape per `HapMsgType`

## Recent changes

- v2 of the wire format adds `ACK_SEQ` (header grew from 10 to 12 B; overhead
  from 12 to 14 B). v1 frames are rejected with `HAP_DECODE_BAD_VERSION`.
- `hap_decode_stream` was added so a single CRC error doesn't poison the
  remainder of the SPI byte stream.
- `hap_make_reply()` is the canonical builder for correlated responses; new
  HAP handlers should use it instead of zero-initialising `HapFrame` by hand.
