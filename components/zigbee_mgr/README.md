# zigbee_mgr — Zigbee Coordinator Orchestrator

P4-side orchestration layer that owns the Zigbee coordinator lifecycle: drives the 9-step ZNP startup, runs the two-task ZCL pipeline, manages the in-memory device pool, runs the synchronous interview, owns the deferred configure queue, and exposes high-level ZCL/ZDO commands. It does not itself decode vendor traffic — the four-path decode pipeline routes frames through `zhc_adapter` (the static-memory ZHC library) plus the small set of legacy in-tree fallbacks.

For the end-to-end air→shadow trace read `docs/ZCL_FLOW.md`. This README is what `zigbee_mgr` itself does.

## Where it sits

```
┌────────── CC2652 ZNP (external) ──────────┐
│   UART @ 115200, MT serial                 │
└────────────────┬───────────────────────────┘
                 │ AREQ + SRSP
                 ▼
        components/znp_driver/
                 │ AREQ callbacks    SREQ helpers
                 ▼
┌──────── components/zigbee_mgr/ ──────────────────────────┐
│ zigbee_mgr.cpp        startup, AF_INCOMING_MSG dispatch  │
│ zcl_commands.cpp      ZCL/ZDO encoders + senders         │
│ zcl_seq.cpp           monotonic ZCL TSN counter          │
│ zigbee_pool.cpp       PSRAM pool + recursive mutex       │
│ zigbee_interview.cpp  9-step join → topology → identity  │
│ zigbee_identity.cpp   late Basic-cluster enrichment      │
│ zigbee_configure_*.cpp post-interview bind/report queue  │
│ zigbee_diagnostics.cpp 32-slot unhandled-frame ring      │
│ zhc_*_bridge.cpp      ZHC adapter glue (decode/send/cfg) │
└────────┬─────────────────────────────────────────────────┘
         │ ZclAttribute (84 B, schema v6)
         ▼
   device_shadow / event_bus / zhc_adapter
```

Two FreeRTOS tasks own the ZCL hot path:

```
znp_rx (driver, prio 6)              zcl_attr (mgr,    prio 5)
   AF_INCOMING_MSG ─► s_zcl_queue ─►  decode pipeline ─► shadow / event_bus
                     depth 16,
                     drop-OLDEST
                     on overflow
```

The split keeps UART RX off the ZHC decoders, NVS, and event-bus fan-out
(QWEN §4 / CODEX §4).

## CMakeLists

```cmake
idf_component_register(
    SRCS "zigbee_mgr.cpp" "zigbee_interview.cpp" "zigbee_interview_utils.cpp"
         "zigbee_identity.cpp" "zigbee_configure_queue.cpp"
         "zcl_commands.cpp" "zcl_seq.cpp"
         "zhc_send_bridge.cpp" "zhc_shadow_bridge.cpp" "zhc_configure_bridge.cpp"
         "zigbee_pool.cpp" "zigbee_diagnostics.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos zap_common zap_store event_bus
             znp_driver device_shadow esp_timer zhc_adapter
)
```

## Coordinator startup — 9 steps

Run by `zigbee_mgr_init`; re-run by `zigbee_mgr_reinit` after a crash.
Each step has a retry+timeout; failure logs the offending step.

| # | Step                                | Notes |
|---|-------------------------------------|-------|
| 1 | HW reset                             | `znp_hw_reset()` — drives nRESET 10 ms |
| 2 | `SYS_PING`                           | Verify NCP is on the wire |
| 3 | `SYS_GET_DEVICE_INFO`                | Read coordinator IEEE |
| 4 | `ZB_WRITE_CONFIGURATION`             | Channels, PAN ID, ext PAN ID |
| 5 | `ZDO_STARTUP_FROM_APP`               | Start the Zigbee stack |
| 6 | Wait `ZDO_STATE_CHANGE_IND` → 9      | Coordinator online |
| 7 | `AF_REGISTER`                        | Endpoint 1 + cluster list |
| 8 | Register AREQ handlers               | `SYS_RESET_IND`, `ZDO_*`, `AF_INCOMING_MSG` (0x81/0x82), `ZDO_TC_DEV_IND` |
| 9 | Lower startup task prio 5→2          | Cede CPU to UART RX + worker |

`s_coordinator_ready`, `s_reset_received`, `s_znp_crashed` are
`std::atomic<bool>` (ZB-F4 fix; previously polled cross-task without
synchronization).

## Decode pipeline — four paths, priority-ordered

`zcl_attr_task` runs four decoders against every frame in `s_zcl_queue`.
**First non-empty result wins.** If all miss, the frame is published as
`ZCL_RAW` and recorded in the `zb_diag` ring.

```
                     ┌── 1 Tuya cluster 0xEF00  → tuya_dp dispatch (zhc_adapter)
AfRawFrame ──────────┤── 2 ZHC VendorRule       → zhc_adapter_try_decode
                     ├── 3 ZHC CommandRule      → zhc_adapter_try_decode (cmd path)
                     └── 4 ZHC FromRule         → zhc_adapter_try_decode (attr path)
                          │ all miss
                          └─► event_bus_publish(ZCL_RAW) + zb_diag_record_unhandled
```

Per-frame side-effects happen on every inbound regardless of decoder:

- `dev->last_seen = now` and `dev->link_quality = lqi` updated in pool.
- Tuya `genTime` (cluster 0x000A, READ_ATTR) gets a synthetic time response — Tuya end-devices gate button reporting on a successful time sync (`zigbee_respond_gen_time`).
- Profile-wide unicasts that didn't disable default response receive a `Default Response` (loop-prevented for the response itself).
- Identity enrichment: every cluster-0x0000 frame is shovelled at `zigbee_identity_on_af_incoming` for late Basic-cluster updates.
- `on_tc_dev_ind` (Trust Center join indication) — **as of 2026-04-25 also seeds the pool** when the IEEE isn't already known; previously ZNP rejoins of unknown devices logged "AF_INCOMING from unknown nwk=…" forever.

### `s_zcl_queue` overflow policy (drop-oldest)

```cpp
static constexpr uint8_t ZCL_QUEUE_DEPTH = 16;
static constexpr uint8_t ZCL_PAYLOAD_MAX = 128;
```

On burst overflow we evict the oldest frame and try the new one once
more. Newer frames carry the values the user actually wants delivered;
the old behaviour (drop-newest) was wrong.

## Public API (`include/zigbee_mgr.h`)

### Lifecycle

```cpp
bool      zigbee_mgr_init();              // run 9-step startup, blocking
bool      zigbee_mgr_reinit();            // re-run startup without one-time setup
bool      zigbee_mgr_crashed();           // unexpected SYS_RESET_IND seen post-init
uint64_t  zigbee_mgr_coordinator_ieee();  // 0 until init completes
bool      zigbee_force_recommission();    // wipe stick's ZNP_HAS_CONFIGURED marker
```

### Join window

```cpp
bool zigbee_permit_join(uint8_t duration_s);  // 0=close, 255=open indefinitely
```

### High-level ZCL helpers

```cpp
bool zigbee_zcl_on_off    (uint16_t nwk, uint8_t ep, uint8_t cmd);                 // 0=Off 1=On 2=Toggle
bool zigbee_zcl_level     (uint16_t nwk, uint8_t ep, uint8_t level,  uint16_t transition_tenths);
bool zigbee_zcl_color_temp(uint16_t nwk, uint8_t ep, uint16_t mireds, uint16_t transition_tenths);

bool zigbee_zcl_read              (uint16_t nwk, uint8_t ep, uint16_t cluster,
                                   const uint8_t* attr_ids_le, uint8_t attr_count);
bool zigbee_zcl_cluster_command   (uint16_t nwk, uint8_t ep, uint16_t cluster, uint8_t cmd_id,
                                   const uint8_t* payload, uint8_t payload_len, uint8_t flags);
bool zigbee_zcl_cluster_command_wait_confirm(uint16_t nwk, uint8_t ep, uint16_t cluster, uint8_t cmd_id,
                                             const uint8_t* payload, uint8_t payload_len,
                                             uint8_t flags, uint32_t confirm_timeout_ms);
bool zigbee_send_default_response(uint16_t nwk, uint8_t ep, uint16_t cluster,
                                  uint8_t incoming_fc, uint16_t mfg_code,
                                  uint8_t tsn, uint8_t cmd_id, uint8_t status);
bool zigbee_respond_gen_time     (uint16_t nwk, uint8_t ep, uint8_t tsn,
                                  const uint8_t* req_body, size_t req_body_len);
```

The `_wait_confirm` variant blocks on `AF_DATA_CONFIRM` (via
`znp_confirm`) — surfaces real MAC delivery failures instead of silently
declaring success when the frame never lands. Used by the configure
bridge for Tuya-style devices that drop too easily.

### Tuya / vendor activation

```cpp
bool zigbee_tuya_magic_packet         (uint16_t nwk, uint8_t ep);   // genBasic probe
bool zigbee_miboxer_fut089z_finalize  (uint16_t nwk, uint8_t ep);   // genGroups 0xF0 + tuyaSetup
```

### ZDO

```cpp
bool zigbee_zdo_bind  (uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep);
bool zigbee_zdo_unbind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep);
bool zigbee_leave_req (uint16_t nwk, uint64_t ieee);
```

### Device management

```cpp
bool zigbee_interview_trigger        (uint64_t ieee);   // re-run full interview
void zigbee_interview_flush_join_queue();               // drop stale joins on reinit
bool zigbee_pool_remove              (uint64_t ieee);   // hard-remove from in-memory pool
```

`zigbee_interview_trigger` returns the real scheduling result (CODEX §7
fix) — REST callers see a 500 instead of a silent success when the
queue is full.

## In-memory pool (`include/zigbee_pool.h`)

PSRAM-backed (200 × 522 B = ~104 KB), fall-back to internal RAM if PSRAM
alloc fails. Two open-addressing hash tables (256 slots each, Knuth's
multiplicative hash) provide O(1) lookups by IEEE and NWK.

```cpp
void        zigbee_pool_init();
void        zigbee_pool_lock();
void        zigbee_pool_unlock();
void        zigbee_pool_mark_dirty();   // rebuild hash on next lookup

ZapDevice*  pool_find_by_ieee(uint64_t ieee);   // O(1)
ZapDevice*  pool_find_by_nwk (uint16_t nwk);    // O(1)
ZapDevice*  pool_add ();                        // append, nullptr if full
void        pool_remove(uint16_t idx);          // rare — rebuilds hash
inline ZapDevice* pool_all  () { return s_pool; }
inline uint16_t   pool_count() { return s_pool_count; }
```

**Concurrency:** the pool is shared between the interview task, the ZCL
task, the REST handlers via the backend, and the HAP dispatcher. The
public lookups take a recursive internal mutex. Use the advisory pair
`zigbee_pool_lock/unlock` around any sequence that must be atomic
(e.g. find-then-iterate-endpoints), and call `zigbee_pool_mark_dirty()`
after **any** in-place mutation to a hashed field (`ieee_addr`,
`nwk_addr`) — forgetting this causes rejoins to fail with
"AF_INCOMING from unknown nwk=…" because the hash still maps the old
nwk to the slot.

## Configure queue (`include/zigbee_configure_queue.h`)

Step 3 of the backend-agnostic device lifecycle. Separates the slow,
retry-friendly **configure** (binding, `Configure Reporting` setup,
cluster-specific writes, vendor activation) from the synchronous
interview. A dedicated worker:

- never blocks the interview task on a flaky bind round-trip;
- retries failures with exponential backoff;
- dedupes via `ConfigureState::DONE` so rejoins don't spam the
  binding-table on constrained devices.

```cpp
void zigbee_configure_init();                 // start the worker
void zigbee_configure_enqueue(uint64_t ieee); // idempotent
```

## Diagnostics (`include/zigbee_diagnostics.h`)

32-slot LRU ring keyed by `(cluster, attr_or_cmd, cluster_specific)`.
Repeats bump a counter rather than flooding the ring.

```cpp
struct ZbUnhandledFrame {
    uint16_t cluster_id;
    uint16_t attr_or_cmd_id;
    uint8_t  cluster_specific;
    uint8_t  _pad;
    uint32_t count;
    uint32_t last_seen_s;
    uint64_t last_ieee;
};

void     zb_diag_init();
void     zb_diag_record_unhandled(uint16_t cluster, uint16_t ac, bool cluster_specific, uint64_t ieee);
uint16_t zb_diag_snapshot         (ZbUnhandledFrame* out, uint16_t max);   // most-recent first
void     zb_diag_reset();
```

Backs `/api/diagnostics/unhandled` so an operator can decide whether a
miss is a genuine pipeline gap or a benign ZCL housekeeping frame.

## Identity enrichment (`include/zigbee_identity.h`)

Late-Basic-cluster path. Sleepy end-devices commonly miss the
synchronous identity read at join. Every inbound cluster-0x0000 frame
is forwarded to a worker queue with zero blocking; the worker promotes
`InterviewState::IDENTITY_PENDING → IDENTITY_READY`, persists the
device once per identity change, and triggers ZHC re-match.

```cpp
void zigbee_identity_init();
void zigbee_identity_on_af_incoming(const uint8_t* af_payload, uint8_t af_payload_len);
```

## Interview helpers (`include/zigbee_interview_utils.h`)

```cpp
uint8_t zigbee_interview_build_basic_probe_order(const ZapDevice& dev,
                                                 uint8_t* out_eps, uint8_t out_cap);
bool    zigbee_parse_basic_identity(const uint8_t* data, uint8_t data_len,
                                    char* model_out, uint8_t model_cap,
                                    char* mfg_out,   uint8_t mfg_cap,
                                    uint16_t* mfg_code_out);
```

Endpoints advertising cluster 0x0000 in their in-cluster list are
probed first (preserves order); the parser tolerates partial frames and
unknown attribute types.

## ZCL TSN (`include/zcl_seq.h`)

```cpp
uint8_t zcl_seq_next();   // wraps 0x01..0xFF; zero is reserved
```

Shared monotonic counter used by every outbound ZCL frame.

## AREQ handlers registered in step 8

| AREQ | Handler | Purpose |
|------|---------|---------|
| `SYS_RESET_IND`            | crash-detect setter         | sets `s_reset_received` / `s_znp_crashed` |
| `ZDO_STATE_CHANGE_IND`     | startup gate / liveness     | state=9 ⇒ `s_coordinator_ready=true` |
| `ZDO_TC_DEV_IND`           | `on_tc_dev_ind`             | join announcement; **seeds pool when IEEE unknown** |
| `ZDO_LEAVE_IND`            | `on_zdo_leave_ind`          | mark `ZAP_DEV_REMOVED`, publish `DEVICE_LEAVE` |
| `ZDO_END_DEVICE_ANNCE_IND` | interview trigger           | new device announced itself |
| `AF_INCOMING_MSG` (0x81)   | `on_af_incoming_msg`        | regular ZCL frame |
| `AF_INCOMING_MSG` (0x82)   | `on_af_incoming_msg`        | extended (security) variant |
| `AF_DATA_CONFIRM` (0x80)   | `znp_confirm` (driver-side) | MAC delivery status — block-on-confirm path |

## Tasks / priorities

```
znp_rx (driver)        prio 6   UART RX, MT framing, AREQ fanout
znp_worker (driver)    prio 5   TX side, SREQ/SRSP round-trip
zcl_attr (mgr)         prio 5   four-path decode pipeline
zb_interview           prio 4   stepwise interview of new devices
zb_configure           prio 4   bind + report + vendor activation
zigbee_mgr startup     prio 5→2 drops after step 9
```

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| Step 1–9 timeout | log `step N failed`, `zigbee_mgr_init` returns false |
| Unexpected `SYS_RESET_IND` post-init | `s_znp_crashed=true`; supervisor calls `zigbee_mgr_reinit` |
| `s_zcl_queue` overflow | drop oldest, log `evicted oldest` |
| `AF_INCOMING from unknown nwk=…` | nwk hash stale or rejoin pre-fix; expect rebind via `on_tc_dev_ind` |
| `zigbee_pool_remove` after pool full | n/a — soft-removal flag is preferred |
| `zigbee_interview_trigger` queue full | returns `false` — REST surfaces 500 |

## Integration

```cpp
if (!zigbee_mgr_init()) { ESP_LOGE(TAG, "coordinator init failed"); return; }
ESP_LOGI(TAG, "coord IEEE: 0x%016llX", zigbee_mgr_coordinator_ieee());
zigbee_permit_join(60);

zigbee_zcl_on_off(0x1234, 1, 0x01);
zigbee_zcl_level (0x1234, 1, 128, 10);

// Crash supervisor
for (;;) {
    if (zigbee_mgr_crashed()) {
        ESP_LOGW(TAG, "ZNP crash — reinit");
        znp_hw_reset();
        zigbee_mgr_reinit();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

## Recent changes

- **2026-04-25** — `on_tc_dev_ind` now seeds pool when IEEE not present, fixing rejoin loops for devices that left the pool but stayed in the ZNP routing table.
- **2026-04-22** — `s_coordinator_ready` / `s_reset_received` / `s_znp_crashed` migrated to `std::atomic<bool>` (ZB-F4).
- **2026-04-19** — legacy in-tree `zcl_converter` pipeline removed; ZHC adapter is the single decode path. `dispatch_command_rule` / `translate_synthetic_attrs` deleted.
- **2026-04-19** — `flags` byte added to `ZapDevice`; soft-removal via `ZAP_DEV_REMOVED` keeps shadow + name across rejoins.

## Cross-references

- `docs/ZCL_FLOW.md` — full air-to-shadow trace
- `docs/ZNP_API_CONTRACT.md` — list of MT calls/AREQs zigbee_mgr depends on
- `docs/FINDINGS.md` — ZB-F1..F15 (driver/manager findings), CC-F5 (portMAX_DELAY hazards)
- `components/znp_driver/README.md` — UART transport
- `components/zhc_adapter/README.md` — ZHC integration glue
- `components/device_shadow/README.md` — downstream consumer of decoded `ZclAttribute`s
- `components/event_bus/README.md` — `ZCL_RAW` / `DEVICE_LEAVE` / `DEVICE_ATTR` payloads
