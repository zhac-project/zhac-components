# hap_json — JSON Payload Codec for HAP

Encodes and decodes the JSON body that goes inside every `HapFrame.payload`.
There is one `hap_json_encode_<type>` / `hap_json_decode_<type>` pair per
HAP message type — they take caller-supplied byte buffers, no allocation.
Built on ArduinoJson 7. Both chips link this component.

This is the second-largest component in the firmware (~1 280 LOC) because
the wire shape is enumerated rather than reflected. The trade-off: zero
runtime introspection cost, host-testable, and the JSON shape is grep-able
from a single header.

## Where it sits

```
caller ─► hap_json_encode_<type>(buf, cap, &len, struct) ─► HapFrame.payload ─► hap_session_send
                                                                  │
caller ◄─ hap_json_decode_<type>(payload, len, struct) ◄─ HapFrame ◄─ hap_session_on_receive
```

Used by:

- `zhac-main-core/main/hap_dispatch.cpp` — every per-type handler on the P4
- `zhac-net-core/main/api_handlers.cpp` and friends — REST/WS handlers on the S3
- `components/mqtt_gw/mqtt_gw_p4.cpp` — encodes `HAP_MQTT_PUBLISH` for forwarding
- `components/log_ring` — `HAP_LOG_LINE` codec

## Dependencies (CMakeLists.txt)

`REQUIRES zap_common arduinojson hap_protocol`. `zap_common` provides
`ZapDevice` (used by `device_list` / `device_info`) and the shared
`ZclAttribute` (used by `set_attr` / `bulk`).

## Public API shape

Every encoder follows the pattern:

```cpp
bool hap_json_encode_<type>(uint8_t* buf, size_t cap, uint16_t* out_len,
                             /* type-specific args */);
```

- Returns `true` on success; `*out_len` is set to bytes written.
- Returns `false` on buffer overflow or serialization error; `*out_len`
  unmodified.
- Caller owns `buf`. No allocation inside.

Every decoder follows:

```cpp
bool hap_json_decode_<type>(const uint8_t* payload, uint16_t len,
                             /* type-specific out struct or args */);
```

- Returns `true` on parse + schema match.
- Returns `false` on JSON parse error or missing required key.
- All output struct fields are zeroed before parsing on failure paths.

## Public structs (non-trivial only)

Defined in `include/hap_json.h`. Every non-primitive HAP message has a
typed struct so callers don't pass parameter blizzards.

| Struct                | LoC line | Used by                                   |
|-----------------------|----------|-------------------------------------------|
| `HapSyncInfo`         | :12      | `hap_json_encode/decode_sync*`            |
| `HapHeartbeat`        | :25      | `HEARTBEAT`                               |
| `HapSetAttrReq`       | :93      | `SET_ATTRIBUTE` (uses string-keyed `ZclAttribute`-style fields) |
| `HapAlert`            | :117     | `ALERT`                                   |
| `HapDeviceEvent`      | :129     | `DEVICE_EVENT`                            |
| `HapRuleExecResult`   | :160     | `RULE_EXEC_RESULT`, also reused for `SCRIPT_ACK` |
| `HapRuleSlotInfo`     | :166     | `RULE_LIST_RSP`                           |
| `HapScriptInfo`       | :226     | `SCRIPT_LIST_RSP` (name + size)            |
| `HapOtaChunkHdr`      | :303     | `OTA_CHUNK` (binary header)               |
| `HapOtaStatus`        | :312     | `OTA_STATUS`                              |
| `HapBindReq`          | :333     | `BIND_REQ`                                |
| `HapMqttMsgIn`        | :412     | `MQTT_MSG_IN` (S3→P4 forward of inbound)  |
| `HapMqttPublish`      | :428     | `MQTT_PUBLISH` (P4→S3 forward of outbound)|

Notable size caps (encoded in struct fields): `HapMqttPublish::topic[128]`,
`payload[512]`; `HapMqttMsgIn::topic[64]`, `payload[256]`;
`HAP_SCRIPT_MAX_SRC = 3900` (script source body bound — leaves room for the
JSON wrapper inside `HAP_MAX_PAYLOAD`); `HAP_SCRIPT_NAME_MAX = 24`.

## Wire format per type (selected)

### `SYNC` (0x05)

```json
// Request
{"ver": 2}
// Acknowledgment
{"ver": 2, "chip": "p4", "fw": "v0.7.0"}
```

### `HEARTBEAT` (0x50)

```json
{"up": 3600000, "heap": 120000, "psram": 30000000, "cpu": 15, "pmask": 1}
```

`pmask` bit 0 = Zigbee stack up.

### `DEVICE_LIST` (0x11) — paginated

```json
[
  {"ieee":"0x00124B0012345678", "nwk":4660, "name":"Living Room Lamp",
   "model":"TRADFRI bulb E27",
   "endpoints":[{"ep":1,"in":[0,6,8,768],"out":[5]}],
   "lqi":180, "battery":85}
]
```

`hap_json_encode_device_list` takes an optional `HapJsonLabelResolverFn` so
the encoder can ask the caller for a friendly per-cluster label without
pulling `zhc_adapter` into this layer.

### `SET_ATTRIBUTE` (0x14) and `SET_ACK` (0x15)

```json
// Request
{"ieee":"0x00124B0012345678", "key":"state", "val":"on"}
// Ack
{"ok": true}
// Error
{"ok": false, "err": "device not found"}
```

`val` is polymorphic — the encoder/decoder branches on `ZclAttribute::val_type`
(`VAL_INT`, `VAL_BOOL`, `VAL_STR`).

### `BULK_STATE_UPDATE` (0x60)

```json
{
  "type":"device_update",
  "ieee":"0x70c59cfffe1d8d7c",
  "lqi":210,
  "last_seen":1712345678,
  "attrs":{"action":"brightness_step_up"}
}
```

Production payload is per-attr — emitted by
`hap_dispatch::on_zcl_attr_for_hap` (P4) via
`hap_json_encode_device_attr_update`. The S3 bridge forwards the payload
verbatim as the WS event `attr.bulk` (SPA dispatcher in
`www-spa/src/stores/devices.js` patches the matching device row).

The header still ships `hap_json_encode_bulk` (`{"devs":[…]}` array of
`HapDeviceEvent`) for unit tests, but the production batching path
(`flush_bulk` / `bulk_push`) was retired 2026-04-25 (CC-F6 in
`docs/FINDINGS.md`). New batch consumers should use the per-attr shape
above instead.

### `MQTT_PUBLISH` (0x70)

```json
{"topic":"zhac/devices/0x.../state", "payload":"on", "qos":1, "retain":false}
```

`payload` is an arbitrary string (raw bytes are escaped). On the P4 side
this is the outbound forward; on the S3 side, it's both the receive side
of that forward and the self-publish path for log lines.

### `RULE_LIST_RSP` (0x34) / `SCRIPT_LIST_RSP` (0x3A)

```json
{"rules":[{"id":1,"type":0,"enabled":true,"src":"…dsl…"}, …]}
{"scripts":[{"name":"motion","size":1234}, …]}
```

### `LOG_LINE` (0x80)

```json
{"lvl":"I", "tag":"hap", "msg":"hap_master init OK …"}
```

The complete enumeration is in `components/hap_json/include/hap_json.h`
(~440 lines). `git grep '^bool hap_json_'` lists every codec function.

## Important constants

| Constant                | Value | Notes                                   |
|-------------------------|-------|-----------------------------------------|
| `HAP_SCRIPT_MAX_SRC`    | 3900  | Max Lua source bytes inside one frame.  |
| `HAP_SCRIPT_NAME_MAX`   | 24    | Script filename, excluding NUL.         |
| (per-struct fixed sizes — see structs above) | varies | All wire caps are compile-time. |

## Threading and concurrency

Stateless. Every function operates on caller buffers; no globals, no mutex,
no allocation (ArduinoJson uses its own internal pool which is local to
each call). Safe to call from any task.

## Error and failure modes

- Encoder overflow → returns `false`, `*out_len` not written. Caller usually
  drops the frame and logs.
- Decoder parse failure → returns `false`. Caller typically responds with
  `HAP_ERR` or a `SET_ACK` with `ok:false`.
- Truncation in fixed-size fields (e.g. `HapMqttPublish::topic[128]`) is
  silent on the encoder; callers should `strncpy`-style cap before passing
  the struct in.

There are no internal counters or metrics. Diagnose by enabling `D` logs
on the calling component (`hap_dispatch`, `mqtt_gw_p4`, etc.).

## Integration example

Encoding a `DEVICE_JOIN` from the P4:

```cpp
#include "hap_json.h"

uint8_t  buf[64];
uint16_t len = 0;
if (!hap_json_encode_device_join(buf, sizeof(buf), &len,
                                   ieee, nwk)) {
    ESP_LOGE("hap", "device_join encode overflow");
    return;
}
HapFrame f{};
f.type        = HapMsgType::DEVICE_JOIN;
f.seq         = hap_session_next_seq();
f.flags       = 0;
f.payload     = buf;
f.payload_len = len;
hap_session_send(f);
```

Decoding a `SET_ATTRIBUTE` request on the P4:

```cpp
HapSetAttrReq req{};
if (!hap_json_decode_set_attr(frame.payload, frame.payload_len, req)) {
    // reply with SET_ACK ok=false
    return;
}
// req.ieee, req.key, req.val_type, req.int_val / req.str_val
```

Building a `MQTT_PUBLISH` from the P4 side:

```cpp
HapMqttPublish msg{};
strncpy(msg.topic,   "zhac/log/info", sizeof(msg.topic) - 1);
strncpy(msg.payload, "hello",         sizeof(msg.payload) - 1);
msg.qos    = 0;
msg.retain = false;

uint8_t  buf[700];
uint16_t len = 0;
hap_json_encode_mqtt_publish(buf, sizeof(buf), &len, msg);
```

(See `components/mqtt_gw/mqtt_gw_p4.cpp` for the full end-to-end.)

## Cross-references

- `components/hap_protocol/README.md` — frame layout that wraps every payload
- `components/zap_common/include/zcl_attribute.h` — `ZclAttribute` (84 B,
  schema v6) used inside `SET_ATTRIBUTE` and `BULK_STATE_UPDATE`
- `zhac-main-core/main/hap_dispatch.cpp` — primary P4 consumer
- `zhac-net-core/main/api_handlers.cpp` — primary S3 consumer
- `docs/WS_API.md`, `docs/REST_API.md` — how the JSON shapes here surface to clients

## Recent changes

- Added `MQTT_MSG_IN` (0x3E) and `MQTT_PUBLISH` (0x70) codecs to support
  bidirectional MQTT bridging via `mqtt_gw`.
- Added `SCRIPT_RUN_REQ` (0x58) and `SCRIPT_CHECK_REQ`/`_RSP` (0x59/0x5A)
  for one-shot script invocation and parse-only syntax checks.
- `BULK_STATE_UPDATE` (0x60) production payload is the per-attr
  `device_update` shape from `hap_json_encode_device_attr_update`,
  emitted live by `hap_dispatch::on_zcl_attr_for_hap`. The dead
  `bulk_push` / `flush_bulk` batching path was deleted 2026-04-25
  (CC-F6); the array encoder `hap_json_encode_bulk` is kept for tests
  only.
- `ZclAttribute` widened to 84 B (schema v6, post-2026-04-25): `key[28]`,
  `str_val[48]`. Decoders that build a `ZclAttribute` from JSON respect
  the new caps.
