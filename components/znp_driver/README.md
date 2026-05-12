# znp_driver — TI Z-Stack ZNP UART Transport

Synchronous-request / async-event UART transport for TI CC2652 (and CC2530) ZNP coprocessors using the MT (Monitor Test) serial protocol. Single RX task owns the parser; single worker task owns TX and SREQ/SRSP correlation; AREQ frames go straight from RX to a subscriber registry. AF_DATA_REQUEST → AF_DATA_CONFIRM correlation lives in a separate 16-slot ring (`znp_confirm`).

## Where it sits

```
zigbee_mgr / znp_confirm / config bridges
        │  znp_call(cmd0, cmd1, …)        znp_subscribe_areq(cmd0, cmd1, cb)
        ▼                                          ▲
┌─────────────────────── znp_driver ───────────────┴───────────────┐
│  znp_worker.cpp ── owns TX + reply queues, late-SRSP policy      │
│  znp_rx.cpp     ── owns uart_read_bytes, feeds parser            │
│  znp_parser.cpp ── SOF/LEN/CMD0/CMD1/DATA/FCS, streaming         │
│  znp_areq_dispatch.cpp ── subscriber registry (24 slots)         │
│  znp_state.cpp  ── transport state machine + counters            │
│  znp_transport.cpp ── UART + GPIO bring-up, hw_reset             │
│  znp_confirm.cpp ── 16-slot AF_DATA_REQUEST → CONFIRM ring       │
│  znp_driver_shim.cpp ── back-compat for legacy MtFrame API       │
└──────────────────────────────────────────────────────────────────┘
        │   UART_NUM_1, 115200 8N1, no flow control
        ▼
   CC2652 / CC2530 ZNP coprocessor
```

## CMakeLists

```cmake
idf_component_register(
    SRCS         "src/znp_parser.cpp" "src/znp_rx.cpp" "src/znp_worker.cpp"
                 "src/znp_areq_dispatch.cpp" "src/znp_state.cpp"
                 "src/znp_transport.cpp" "src/znp_driver_shim.cpp"
                 "src/znp_confirm.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_driver_uart esp_driver_gpio freertos esp_timer
    PRIV_REQUIRES metrics
)
```

## MT wire format (znp_parser.cpp)

```
+------+------+------+------+----------+-----+
| SOF  | LEN  | CMD0 | CMD1 |  DATA…   | FCS |
+------+------+------+------+----------+-----+
   1B    1B     1B     1B     LEN B      1B
```

| Field | Notes |
|-------|-------|
| `SOF` | constant `0xFE` (`MT_SOF`) |
| `LEN` | data byte count, 0–250 (`MT_MAX_PAYLOAD`) |
| `CMD0`| top 3 bits = type (`SREQ=0x20`, `AREQ=0x40`, `SRSP=0x60`); bottom 5 = subsystem |
| `CMD1`| command id within subsystem |
| `DATA`| LE-packed payload |
| `FCS` | XOR of LEN ⊕ CMD0 ⊕ CMD1 ⊕ each DATA byte |

`SOF` and `FCS` live exclusively in `znp_parser.cpp`. They never appear
in `ZnpFrame` — the public API operates on parsed frames.

### Subsystem IDs

| Value | Subsystem | `ZNP_*` macro |
|-------|-----------|---------------|
| 0x01  | SYS       | `ZNP_SYS`     |
| 0x04  | AF        | `ZNP_AF`      |
| 0x05  | ZDO       | `ZNP_ZDO`     |
| 0x07  | UTIL      | `ZNP_UTIL`    |
| 0x0F  | APP_CNF   | `ZNP_APP_CNF` |

### MT class helpers (znp_driver.h)

```cpp
constexpr uint8_t MT_SREQ(uint8_t sub) { return 0x20 | sub; }
constexpr uint8_t MT_AREQ(uint8_t sub) { return 0x40 | sub; }
constexpr uint8_t MT_SRSP(uint8_t sub) { return 0x60 | sub; }
```

`znp_is_expected_srsp(req_cmd0, req_cmd1, rsp_cmd0, rsp_cmd1)` is
exposed for unit tests + worker — pure function, no side effects.

## Public API

### `include/znp_types.h` — value types

```cpp
struct ZnpFrame {
    uint8_t cmd0, cmd1;
    uint8_t len;                              // data bytes only
    uint8_t data[ZNP_MAX_DATA_LEN];           // owning buffer
};

enum class ZnpStatus : uint8_t {
    OK, TIMEOUT, UART_TX_ERROR, RESET_DURING_CALL,
    TRANSPORT_DOWN, INTERNAL_ERROR, UNEXPECTED_RESPONSE
};

enum class ZnpTransportState : uint8_t { Down, Booting, Init, Up, Recovering, Error };

struct ZnpTransportStats {
    uint32_t tx_sreq_count, rx_srsp_count, rx_areq_count;
    uint32_t resets_seen, timeouts;
    uint32_t unexpected_srsp, late_srsp;
    uint32_t bad_frames, tx_errors;
    uint32_t recoveries;
    uint32_t duplicate_areqs;   // dropped by ZDO response dedup ring
};

using ZnpAreqHandler = std::function<void(const ZnpFrame&)>;
```

`ZnpFrame` carries its own `data[]` buffer — fixes the "two callers
stomp on the same s_srsp_buf" race the old single-buffer design had.

### `include/znp_transport.h` — preferred API

```cpp
void      znp_transport_start();                       // bring-up; idempotent
ZnpStatus znp_call         (uint8_t cmd0, uint8_t cmd1,
                            const uint8_t* data, uint8_t data_len,
                            ZnpFrame& srsp_out, uint32_t timeout_ms);
ZnpStatus znp_call_retry   (uint8_t cmd0, uint8_t cmd1,
                            const uint8_t* data, uint8_t data_len,
                            ZnpFrame& srsp_out,
                            uint32_t timeout_ms, int max_attempts);
void      znp_subscribe_areq(uint8_t cmd0, uint8_t cmd1, ZnpAreqHandler cb);
void      znp_areq_dispatch (const ZnpFrame& f);       // exposed for tests
void      znp_hw_reset();                              // 10 ms low pulse on nRESET
ZnpTransportState znp_get_state();
ZnpTransportStats znp_get_stats();
bool      znp_is_expected_srsp(uint8_t r_cmd0, uint8_t r_cmd1,
                               uint8_t s_cmd0, uint8_t s_cmd1);
```

### `include/znp_driver.h` — legacy API + parser primitives

Still works through the shim — no callers need to migrate.

```cpp
uint8_t mt_fcs(...);
size_t  mt_encode(...);
MtDecodeResult mt_decode(...);

void znp_driver_init();      // alias for znp_transport_start
bool znp_sreq      (const MtFrame& req, MtFrame& srsp_out, uint32_t timeout_ms = 2000);
bool znp_sreq_retry(const MtFrame& req, MtFrame& srsp_out,
                    uint32_t timeout_ms = 2000, int max_attempts = 3);
void znp_register_areq(uint8_t cmd0, uint8_t cmd1, MtAreqCallback cb);
void znp_hw_reset();

void znp_set_wire_trace(bool enabled);   // INFO-level hex dump under tag "znp_wire"
bool znp_get_wire_trace();
```

The shim allocates its `MtFrame` payload from a per-task buffer so two
callers can hold replies simultaneously.

### `include/znp_confirm.h` — MAC delivery confirmation ring

```cpp
int  znp_confirm_reserve(uint8_t trans_id);  // -1 if 16-slot ring full
int  znp_confirm_wait   (int slot, uint32_t timeout_ms);  // 0x00 success, 0xF0 expired, -1 timeout
void znp_confirm_release(int slot);
```

Caller MUST reserve before sending — a fast confirm dropped before a
slot exists is unrecoverable. Internally, `znp_confirm` subscribes to
AREQ `cmd0=0x44 cmd1=0x80` (`AF_DATA_CONFIRM`) and matches by
`trans_id` (3rd payload byte).

## Hardware

UART `UART_NUM_1`, 115200 8N1, no flow control. Pins via Kconfig
(`menuconfig → ZHAC Zigbee NCP Backend → ZHAC ZNP GPIO Pins`):

| Signal | Default | Kconfig key |
|--------|---------|-------------|
| TX     | GPIO16  | `CONFIG_ZHAC_ZNP_UART_TX_GPIO`  |
| RX     | GPIO17  | `CONFIG_ZHAC_ZNP_UART_RX_GPIO`  |
| nRESET | GPIO28  | `CONFIG_ZHAC_ZNP_UART_RST_GPIO` |
| BSL    | GPIO29  | `CONFIG_ZHAC_ZNP_UART_BSL_GPIO` |

`znp_hw_reset()` drives nRESET low for 10 ms then releases.

## Sizing

| Symbol            | Value | Source |
|-------------------|-------|--------|
| `MT_MAX_PAYLOAD`  | 250   | znp_driver.h |
| `MT_OVERHEAD`     | 5     | SOF + LEN + CMD0 + CMD1 + FCS |
| AREQ subscribers  | 24    | znp_areq_dispatch.cpp |
| Confirm slots     | 16    | znp_confirm.cpp |

## Threading

```
caller task                       worker task              rx task
───────────                       ───────────              ───────
znp_call() ──┐                                           uart_read_bytes
             └► s_request_q ─► dequeue ─► encode ─► TX   ─► MtStreamParser
                                           │               │
                                           ▼               ▼
                                      wait s_wake_q  on_frame() classifies
                                           ▲             │
                        deliver_srsp / reset_ind  ◄──────┤
                                                         │
                        znp_areq_dispatch() ◄────────────┘
                  reply_queue (per call)
                              ▲
                              └── send ZnpReply
```

No global SRSP buffer; every cross-task hand-off is a FreeRTOS queue
carrying an owning `ZnpFrame` value. AREQ dispatch never touches the
worker — async traffic and synchronous bookkeeping cannot interfere.

`znp_subscribe_areq` **replaces** an existing entry for the same
`(cmd0, cmd1)` rather than appending — this contradicts the historical
"all fire" wording (ZB-F8); update before relying on multi-subscription.

## Late / unexpected SRSP policy

| Situation                                | Behaviour |
|------------------------------------------|-----------|
| SRSP matches active request              | delivered to the waiting caller |
| SRSP arrives with no active request      | `stats.late_srsp++`, log W, drop |
| SRSP arrives with mismatched subsys/cmd1 | `stats.unexpected_srsp++`, log W, drop |
| Bad FCS / bad SOF                        | `stats.bad_frames++`, log W, drop |
| Caller's request timed out               | reply is **never** resurrected by a late SRSP |

## Reset handling

- `SYS_RESET_IND` is classified by the RX task as a distinct transport event.
- If a request is in flight, the worker returns `ZnpStatus::RESET_DURING_CALL` and bumps `stats.recoveries`.
- The state machine moves to `Up` on any `SYS_RESET_IND` — the NCP is by definition ready immediately afterward.

## AF_DATA_REQUEST → AF_DATA_CONFIRM correlation

`znp_call` only confirms the NCP **accepted** the request (SRSP). MAC
delivery happens later as `AF_DATA_CONFIRM` AREQ. The `znp_confirm` ring
correlates by `trans_id`:

```cpp
const uint8_t tsn = zcl_seq_next();
int slot = znp_confirm_reserve(tsn);
// build + send AF_DATA_REQUEST with this tsn …
int status = znp_confirm_wait(slot, /*timeout_ms=*/1000);
if (status < 0)        { /* MAC timeout */ }
if (status == 0xF0)    { /* ZMacTransactionExpired */ }
```

`znp_confirm_reserve` MUST come before transmit; a confirm that arrives
before any slot is waiting is dropped. ZB-F9 in `docs/FINDINGS.md`
flags that current ZCL write paths don't yet reserve — block-on-confirm
is currently used only by the Tuya/MiBoxer configure bridge
(`zigbee_zcl_cluster_command_wait_confirm`).

## Diagnostics

```cpp
auto s = znp_get_stats();
// s.tx_sreq_count, s.rx_srsp_count, s.rx_areq_count
// s.timeouts, s.late_srsp, s.unexpected_srsp
// s.bad_frames, s.tx_errors
// s.resets_seen, s.recoveries, s.duplicate_areqs

znp_set_wire_trace(true);   // pairs TX/RX hex dumps under tag "znp_wire"
```

Surfaced on the S3 via `/api/info` (transport state + counters).

## Failure modes

| Condition | Status / behaviour |
|-----------|--------------------|
| Request queue refused (capacity exhausted) | `ZnpStatus::TRANSPORT_DOWN` |
| UART TX error                              | `ZnpStatus::UART_TX_ERROR` |
| No SRSP in `timeout_ms`                    | `ZnpStatus::TIMEOUT`, `stats.timeouts++` |
| `SYS_RESET_IND` mid-call                   | `ZnpStatus::RESET_DURING_CALL`, `stats.recoveries++` |
| AREQ subscribers full (>24)                | log E, handler dropped |
| Confirm ring full (>16)                    | `znp_confirm_reserve` returns -1 |
| Bad FCS frame                              | `stats.bad_frames++`, frame discarded |
| Duplicate ZDO response AREQ                | `stats.duplicate_areqs++`, dedup-ring drop |

## Integration

```cpp
#include "znp_transport.h"

znp_transport_start();          // idempotent
znp_subscribe_areq(MT_AREQ(ZNP_SYS), 0x80, on_sys_reset_ind);

ZnpFrame rsp{};
auto st = znp_call(MT_SREQ(ZNP_SYS), /*cmd1 PING*/0x01,
                   nullptr, 0, rsp, /*timeout_ms*/2000);
if (st == ZnpStatus::OK) {
    uint16_t caps = rsp.data[0] | (rsp.data[1] << 8);
}
```

## Cross-references

- `docs/ZNP_API_CONTRACT.md` — exhaustive MT call/AREQ surface zigbee_mgr depends on
- `components/zigbee_mgr/README.md` — primary consumer
- `components/zigbee_backend/README.md` — backend wrapper that bridges to `device_backend`
- `components/ezsp_driver/README.md` — alternative Silabs path
- `components/c6_driver/README.md` — Espressif ESP32-C6 NCP scaffold (planned third backend)
- `docs/FINDINGS.md` — ZB-F8 (`subscribe_areq` replace vs append), ZB-F9 (confirm ring underused)
