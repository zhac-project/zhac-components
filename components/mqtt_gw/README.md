# mqtt_gw — MQTT Gateway (dual-target)

The single component that lets any task on either chip publish to MQTT. The
S3 build wraps `esp-mqtt`; the P4 build is a thin shim that forwards every
publish over HAP to the S3, which actually talks to the broker. Same header
on both sides — callers don't need to know which chip they're on.

## Where it sits

```
S3                                     P4
──                                     ──
caller (REST/WS, log_sinks,             caller (Lua scripts via zhac.publish,
  ws_event_broadcast, ...)                simple_rules PUBLISH action,
        │                                  log_sinks_p4 → log_ring, …)
        ▼                                       │
mqtt_gw_publish (mqtt_gw_s3.cpp)                ▼
        │                                mqtt_gw_publish (mqtt_gw_p4.cpp)
        │ esp_mqtt_client_publish               │ HapMqttPublish encode
        ▼                                       │
[esp_mqtt task]                                  ▼
        │                                hap_session_send → hap_slave_send
        ▼                                       │
[broker]  ◀── DATA ── MQTT_EVENT_DATA ───┐      ▼
        ▲                                │   SPI to S3
        │                                │      │
mqtt_gw_set_rx_callback ────► RX cb ─────┤      ▼
                                        S3 hap_dispatch handles MQTT_PUBLISH
                                        and calls mqtt_gw_publish (S3 side)
```

A publish from the P4 makes one full round trip: P4 → SPI → S3 → broker.
There is no broker client on the P4. Conversely, inbound MQTT data on the S3
is dispatched only to the local RX callback; if the P4 needs to react to it,
the S3 code path forwards it over HAP using `MQTT_MSG_IN` (0x3E), but that
forward path lives outside this component.

## Dependencies (CMakeLists.txt)

The CMake file branches on `IDF_TARGET` (`mqtt_gw/CMakeLists.txt:3-11`):

| Build  | SRCS                | REQUIRES                                         |
|--------|---------------------|--------------------------------------------------|
| S3     | `mqtt_gw_s3.cpp`    | `mqtt freertos`                                  |
| P4     | `mqtt_gw_p4.cpp`    | `hap_slave hap_session hap_json hap_protocol freertos` |

`mqtt_gw.cpp` is a 6-line stub. NVS is read by the firmware boot path
(`zhac-net-core/main/main.cpp`) — this component only consumes
already-loaded settings via `mqtt_gw_configure()`.

## Kconfig

`mqtt_gw/Kconfig`:

```kconfig
config MQTT_BROKER_URL
    string "MQTT broker URL"
    default "mqtt://localhost"
```

This is a build-time default only — the operator overrides it at runtime
via `/api/settings`, which writes to NVS namespace `mqtt_cfg`.

## Important constants

| Constant            | Value      | Notes                                       |
|---------------------|------------|---------------------------------------------|
| `MQTT_MAX_FAILS`    | 20         | Auto-disable after this many MQTT_DISCONNECT events in a row (`mqtt_gw_s3.cpp:29`). Tolerates WiFi blips. |
| `s_root_topic` cap  | 32 chars   | `mqtt_gw_s3.cpp:23`                         |
| `s_broker_url` cap  | 128 chars  | `mqtt_gw_s3.cpp:22`                         |
| `s_client_id` cap   | 32 chars   | `mqtt_gw_s3.cpp:23`. Empty → auto from MAC. |
| `s_sub_filter` cap  | 128 chars  | One subscription filter (single subscriber).|
| `HapMqttPublish` caps | `topic[128]`, `payload[512]` | per-frame on the P4 wire |

`MQTT_BROKER_URL` from Kconfig is the boot fallback only — `mqtt_gw_configure`
overwrites it.

## Topic structure

z2m-compatible. The root prefix is configurable (default `"zhac"`); operator
can run two controllers on one broker by giving each a unique root.

```
<root>/devices/<ieee>/attributes/<key>   device attribute updates
<root>/devices/<ieee>/availability       online / offline
<root>/bridge/config                     bridge configuration
<root>/bridge/request                    inbound requests
<root>/bridge/response                   outbound responses
<root>/log/<level>                       log lines (mqtt sink)
<root>/status                            system status
```

Use `mqtt_gw_format_topic(buf, cap, "log/info")` to compose a topic; it
joins the configured root and suffix with `/` and returns bytes written
(or -1 on overflow).

## Public API (`include/mqtt_gw.h`)

```cpp
typedef void (*mqtt_rx_cb_t)(const char* topic,   int topic_len,
                              const char* payload, int payload_len);

void mqtt_gw_init();   // S3 logs intent; P4 logs that publishes route via HAP.

// Publish. payload_len is REQUIRED — HAP payloads are not NUL-terminated, and
// strlen() on raw bytes walks uninitialised memory and stalls the outbox
// when the bogus length is huge. For NUL-terminated strings, pass strlen()
// yourself.
//   S3 side: esp_mqtt_client_publish to broker.
//   P4 side: hap_json_encode_mqtt_publish + hap_session_send.
void mqtt_gw_publish(const char* topic, const char* payload, size_t payload_len,
                      int qos, bool retain);

// S3-only: re-init the client with a new broker URL. P4: no-op.
void mqtt_gw_set_broker_url(const char* url);

// S3-only: start / stop the client without changing the URL. Use
// `mqtt_gw_start` after flipping `mqtt_enabled` at runtime so the change
// takes effect without a reboot. P4: no-ops.
void mqtt_gw_start();
void mqtt_gw_stop();

// S3-only: stage broker URL / root / client-id WITHOUT starting the client.
// Boot path uses this so esp-mqtt doesn't spin up before STA is up — the
// connect-then-disconnect loop would otherwise trip MQTT_MAX_FAILS in a few
// seconds. Empty / null args leave the corresponding field untouched.
void mqtt_gw_configure(const char* url, const char* root, const char* cid);

// S3-only: call from the WiFi STA-got-IP handler. Idempotent.
void mqtt_gw_on_sta_up();

// S3-only diagnostics.
bool mqtt_gw_is_connected();
bool mqtt_gw_is_active();              // true if client exists (not auto-disabled)

// S3-only inbound. Single global RX callback (single subscriber). `topic` is
// NOT NUL-terminated — use topic_len.
void mqtt_gw_set_rx_callback(mqtt_rx_cb_t cb);
void mqtt_gw_subscribe(const char* topic_filter, int qos);

// S3-only client-identity tweaks. Both NVS-persisted under `mqtt_cfg`.
// set_client_id triggers a live restart; set_root_topic does not (callers
// re-emit the next published topic via mqtt_gw_format_topic).
void mqtt_gw_set_client_id(const char* id);     // default zhac-<last-4-mac>
void mqtt_gw_set_root_topic(const char* root);  // default "zhac"

// Topic helpers (both sides).
int  mqtt_gw_format_topic(char* out, size_t cap, const char* suffix);
const char* mqtt_gw_get_root_topic(void);       // never returns null
```

P4 stubs return `false` / `0` / no-op for the S3-only functions, so
cross-platform callers can ignore the chip distinction.

## NVS storage

All MQTT settings live in NVS namespace `mqtt_cfg`, written by REST handlers
and re-read at boot in `zhac-net-core/main/main.cpp`.

| Key          | Type | Description                                         |
|--------------|------|-----------------------------------------------------|
| `enabled`    | u8   | 0/1; gates whether the client starts at all         |
| `broker_url` | str  | Full MQTT URI (e.g. `mqtt://192.168.1.10:1883`)      |
| `root_topic` | str  | Topic prefix (32 chars max)                         |
| `client_id`  | str  | MQTT client id (32 chars max)                       |

## Threading and concurrency

S3:
- Event handler `mqtt_event_handler` runs on the esp-mqtt task. The RX
  callback is invoked from there — keep it short or pass through a queue.
- `mqtt_gw_publish` calls `esp_mqtt_client_publish`, which is internally
  thread-safe (esp-mqtt queues the publish to its own task).
- `restart_client` tears down and recreates the client; only safe to call
  from non-mqtt tasks.

P4:
- `mqtt_gw_publish` builds a `HapMqttPublish`, encodes it, and queues it via
  `hap_session_send` → `hap_slave_send`. Both producers are non-blocking and
  thread-safe (see `components/hap_slave/README.md`). Stack-allocated buffers
  inside the function — no shared state.

There is no internal mutex in this component (despite an older note that
claimed one); serialization is delegated to the underlying esp-mqtt and
hap_slave queues.

## Error and failure modes

| Log line                                                  | Meaning                                                  |
|-----------------------------------------------------------|----------------------------------------------------------|
| `I MQTT connected`                                        | esp-mqtt has TCP+CONNACK with broker.                    |
| `I MQTT subscribed to <filter> qos=N`                     | After connect (or after `mqtt_gw_subscribe` while up).   |
| `W MQTT disconnected (fail K/20)`                         | Reconnect underway. esp-mqtt does its own backoff.       |
| `E MQTT auto-disabled after 20 consecutive failures`      | `MQTT_MAX_FAILS` exceeded; client destroyed. Operator must `mqtt_gw_start` (or reboot) to retry. Prevents log floods on a permanently bad URL. |
| `E mqtt init failed`                                      | `esp_mqtt_client_init` returned NULL — usually bad URL. |
| `E MQTT error type=N`                                     | TLS, transport, or DNS error from esp-mqtt event.        |
| `E mqtt_publish encode failed topic=…` (P4)               | `hap_json_encode_mqtt_publish` overflowed the 700 B stack buffer. Frame dropped. |
| `D MQTT_PUBLISH forwarded topic=… qos=N retain=N` (P4)    | Successful P4→S3 forward.                                |

## Integration example

S3-side initialization at boot:

```cpp
#include "mqtt_gw.h"

mqtt_gw_init();

// Boot path: stage from NVS without starting the client.
mqtt_gw_configure(broker_url, root_topic, client_id);
mqtt_gw_set_rx_callback(on_mqtt_message);
mqtt_gw_subscribe("zhac/+/+/set", 1);

// Later, in the WiFi STA-got-IP handler:
mqtt_gw_on_sta_up();
```

P4-side fire-and-forget publish (e.g. from a Lua script):

```cpp
const char* msg = "device booted";
mqtt_gw_publish("zhac/status", msg, strlen(msg), 0, false);
```

Composing a topic against the configured root:

```cpp
char topic[64];
if (mqtt_gw_format_topic(topic, sizeof(topic), "log/info") < 0) {
    return;  // overflow
}
mqtt_gw_publish(topic, msg, strlen(msg), 0, false);
```

## Cross-references

- `components/hap_json/README.md` — `HapMqttPublish` / `HapMqttMsgIn` shapes
- `components/hap_slave/README.md` — P4 outbound transport
- `components/hap_master/README.md` — S3 inbound transport
- `zhac-main-core/main/hap_dispatch.cpp` — handles `MQTT_PUBLISH` (0x70)
  on the S3 side after forward; handles `MQTT_MSG_IN` (0x3E) on the P4 side
- `docs/REST_API.md` — `/api/settings` writes the `mqtt_cfg` NVS keys
- `docs/WS_API.md` — log lines can be teed to MQTT via `log_sinks`

## Recent changes

- Auto-disable cap raised from 3 to **20** consecutive `MQTT_DISCONNECT`
  events — the previous cap was aggressive enough that one reconnect-loop
  during AP→STA transition would permanently disable MQTT until reboot
  (`mqtt_gw_s3.cpp:29` + comment).
- `mqtt_gw_publish` signature gained an explicit `payload_len` parameter.
  Callers must pass the exact byte count; `strlen()` on the raw HAP payload
  is unsafe and was the cause of stalled outboxes (header comment in
  `include/mqtt_gw.h:11-16`).
- Added `mqtt_gw_format_topic`, `mqtt_gw_get_root_topic`,
  `mqtt_gw_set_root_topic`, `mqtt_gw_set_client_id`,
  `mqtt_gw_configure`, `mqtt_gw_on_sta_up`, `mqtt_gw_is_active`,
  `mqtt_gw_start`, `mqtt_gw_stop` — the API surface roughly tripled to
  support runtime reconfiguration and multi-controller deployments on a
  shared broker.
- The boot path now stages MQTT config without starting the client; the
  client only spins up after STA-got-IP, avoiding the connect-disconnect
  loop that used to trip the auto-disable counter during AP+STA fallback.
