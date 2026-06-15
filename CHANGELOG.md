# Changelog

All notable changes to `zhac-components` are documented in this file. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow the platform-wide `vYYYYMMDDVV` scheme tagged from
`zhac-platform`.

## [Unreleased]

### Fixed

- **Float attributes display correctly (`VAL_FLOAT`).** Floats were stored as
  VAL_INT scaled ×100, indistinguishable from a genuine integer, so the JSON
  encoders guessed which to unscale via a hardcoded float-key list — an integer
  humidity got ÷100 (→0.49) on the live path while a float temperature showed
  ×100 (→2870) on the snapshot path. Added a real `VAL_FLOAT` tag (int_val still
  ×100 — no struct/NVS change): the shadow bridge tags floats VAL_FLOAT and every
  JSON value-emitter formats by type (VAL_FLOAT → ÷100, else raw); the float-key
  guess is deleted. Rules/Lua keep the ×100 integer convention (the matcher
  normalises VAL_FLOAT→VAL_INT, so numeric triggers still fire). Adds
  `zcl_attr_set_float` + host tests.

## [v2026061501]

### Changed

- **hap_json — `HapSyncInfo.fw_ver` 16→32 bytes.** The SYNC fw_ver field now
  carries a git-describe firmware version (e.g. `v2026061501`, or
  `v2026061501-3-gabc1234` when ahead of a tag) instead of the old `"0.4.0"`
  protocol string, so it can exceed 16 chars. Decode stays `sizeof`-based.

## [v2026061401]

### Fixed

- **hap_session — device.list/get/set wedge after hours of uptime (uint16
  high-water wrap)** — the receive-side dedup high-water that gates
  `is_stale_behind_window` advanced only on accepted NEEDS_ACK frames, while the
  peer's seq counter advances on *every* frame it sends. The only P4→S3
  NEEDS_ACK frames (DEVICE_LIST/DEVICE_INFO/SET_ACK) are rare; heartbeats + attr
  updates (NO_ACK) raced the counter ahead until the high-water lagged the live
  seq by >32768 (half the uint16 space). `seq_diff` then wrapped and misjudged a
  *fresh* reply as "behind the window", silently dropping it (re-ACK, no
  dispatch); because the drop preceded the high-water advance, the lag never
  recovered — a permanent wedge where heartbeats/`*_RSP` still flowed (link
  looked half-alive) but device.list/get/set timed out forever (observed at 4h
  and 10h uptime, both chips continuously up). Fix: advance the high-water on
  every inbound frame (`note_peer_seq`), pinning the lag to SPI-pipeline depth.
  Also reset the receive dedup on a received SYNC, so a genuine peer restart
  (seq rewind) can't wedge it either. Regression tests added.

## [v2026061302] - 2026-06-13

### Fixed

- **zigbee_mgr (rejoin re-read)** — a device that rejoins with a new nwk
  address now re-reads its state, so its device-shadow repopulates instead of
  staying empty (web UI "no states"). Root cause was a contradiction between
  two existing sites: the interview **rejoin fast-path**
  (`zigbee_interview.cpp`, `InterviewState::IDENTITY_READY`) updates the
  device's `nwk_addr` and calls `zigbee_configure_enqueue()` to re-run
  binds/reporting/reads on rejoin — but `task_configure`
  (`zigbee_configure_queue.cpp`) dedups out any device already in
  `ConfigureState::DONE`, making that re-enqueue a silent no-op. A device is
  marked `DONE` once its configure steps were *sent* without a transport error
  (not verified-answered): a device that joined, started configure, then LEFT
  mid-sequence had its initial `zcl_read`s fired at the since-departed nwk, was
  still marked `DONE`, and on rejoin (new nwk) was skipped — so it never
  re-read and the shadow stayed empty. Fix: the rejoin fast-path now resets a
  `DONE` device's `configure_state` to `PENDING` (and `configure_attempts` to
  0) **atomically under `zigbee_pool_lock()`**, alongside the `nwk_addr`
  fix-up, so the re-enqueue genuinely re-runs on the new live nwk. The reset is
  transient and intentionally **not** persisted — the configure worker
  re-marks `DONE`/`FAILED` and persists the real outcome (mirrors the existing
  "Mark IN_PROGRESS … not persisted" pattern). Surgical and gated to the rejoin
  fast-path only: the `DONE`-dedup in `task_configure` is untouched, so every
  other enqueue caller (e.g. `zigbee_identity.cpp` on match) is still protected
  from duplicate binds / `STATUS_TABLE_FULL` on already-configured devices.
  Needs a hardware soak (rejoin a device with a new nwk, confirm states
  populate). Follow-up (not in this fix): a defensive guard so configure does
  not mark `DONE` when the device left mid-sequence.

- **hap_json (HOTFIX)** — `hap_json_encode_device_list` now PAGES the device
  list. A full fleet's JSON cannot fit one SPI frame (`HAP_MAX_PAYLOAD` =
  4096), and a host test pins the overflow at **16 realistic devices** (15 fit
  at 4003 B, ~266 B/device). The old all-or-nothing encoder returned `false`
  there, so the P4 logged "encode failed", sent nothing, and the S3 device
  list timed out for anyone with ~15+ devices. The encoder gained an optional
  `start_index` / `next_index` paging pair: it fills the frame device-by-device
  from `start_index`, stops before overflow at ~90 % of cap, and reports the
  next cursor in a `{"next":N,"devices":[...]}` envelope (`next` == device
  count = done sentinel). The `devices[]` element shape is byte-identical to
  before. Forward progress is guaranteed (a single over-budget device is still
  emitted alone and the cursor advances — never `next == start` with zero
  encoded, so a paging caller can't spin). Passing `next_index = nullptr`
  keeps the legacy single-frame behaviour. New host test
  `test/host/test_devlist_paging.cpp` confirms the threshold and proves
  split + reassembly (30 devices → 4 chunks → full list), single-chunk and
  empty-list edges.

### Fixed — Low (P5 findings tail, T32)

Conservative LOW-tail sweep of `zhac-components`. Most LOW rows were already
resolved incidentally during the P0–P4 batches (verified against current code);
only the genuinely-open, zero-risk quick wins below were changed. Concurrency /
hot-path / design-required LOW rows were deliberately left for a dedicated pass.

- **zhc_adapter** (LOW DOC, FINDINGS §9, `zhc_adapter.h` `zhac_adapter_try_decode`
  doc): the header claimed decode output "is logged at INFO level; nothing flows
  into device_shadow / the event bus" — stale. Each emitted key/value is in fact
  forwarded to `device_shadow_process` via `zhc_shadow_bridge.cpp:91` (debounce/
  throttle/NVS pipeline + event-bus publish). Comment corrected to match the live
  forwarding path. Doc-only; no behaviour change.
- **zap_common** (LOW DUP, FINDINGS §8, `task_stacks.h:116` `task_stack_size_for`):
  replaced the hand-rolled character-by-character string compare with
  `std::strcmp` (added `<cstring>`). Identical semantics, one fewer open-coded
  loop to maintain.
- **mqtt_gw** (LOW SMELL, FINDINGS §7, `mqtt_gw_p4.cpp:49` publish payload cap):
  a publish payload larger than the fixed 512 B `HapMqttPublish::payload` was
  silently truncated (producing partial / invalid-JSON on the wire). Added a
  one-shot (rate-limited) `ESP_LOGW` so an over-large publish is diagnosable;
  the cap + NUL-terminate behaviour is unchanged.

### Fixed — Medium (P4 findings batch, T31 HAP leftovers)

- **hap_protocol** (MED, FINDINGS HAP, `hap_protocol.cpp` `hap_decode_stream`
  resync scan): off-by-one in the forward resync loop bound. The loop reads
  `buf[i+2]` but stopped at `i + 3 < len`, so a candidate preamble whose 3rd
  byte landed on the final buffer index was missed. Tightened to `i + 2 < len`
  (the true last startable position), so the scan now covers every full-preamble
  start position including the tail.
- **hap_protocol** (MED, FINDINGS HAP, `hap_protocol.cpp` `hap_decode_stream`
  no-candidate consume): when the scan finds NO candidate preamble anywhere, the
  old code left 3 trailing bytes (`len - 3`), re-presenting the same undecodable
  head every call (slow re-parse loop). With the scan bound fixed to cover the
  tail, a "no candidate" verdict is authoritative, so the whole buffer is now
  consumed (`*consumed = len`). A preamble fragment straddling a read boundary is
  acceptably lost on this non-DMA v3 path; the live P4↔S3 link uses the
  fixed-size two-stage DMA decoder, not this scanner.
- **hap_protocol** (DOC, FINDINGS HAP, `hap_protocol.cpp` + `hap_protocol.h`):
  documented the v3 single-frame API (`hap_encode` / `hap_decode` /
  `hap_decode_stream`, CRC8 header) as retained for the host test suite and
  non-DMA / single-shot transports ONLY — it has zero live callers; the wire
  path is the v4 two-stage CRC16 DMA helpers (`hap_encode_stage1` /
  `hap_decode_stage1` / `hap_encode_stage2` / `hap_verify_stage2`). Resolves a
  stale-comment finding by documenting, not deleting (the functions stay).
- **hap_session** (MED, FINDINGS HAP, `hap_session.cpp` `STALE_BEHIND_THRESHOLD`):
  widened the monotonic high-water stale-dup threshold from `WIN_SIZE` (16) to
  `2 * WIN_SIZE` (32) to close a retransmit false-drop. At `WIN_SIZE` the band
  had ZERO margin: a legitimate in-window frame (reordered or retransmitted) can
  sit up to `WIN_SIZE - 1` (15) behind the high-water, and the drop triggered at
  exactly 16 — one boundary reorder false-dropped a real retransmit. The valid
  band is strictly `(WIN_SIZE, SEEN_RING_SIZE)` = (16, 64); `2 * WIN_SIZE` sits
  squarely in the middle with margin on both sides, locked by a `static_assert`.
  The dedup unit test exercises the exact-match ring (same-seq dup), not the
  high-water gap, so no test change was needed.

### Fixed — Medium (P4 findings batch, T30 rule_store)

- **rule_store** (MED, FINDINGS §7, `rule_store.cpp` `rule_store_set_enabled`):
  removed the callerless `rule_store_set_enabled()` from the public API. It
  loaded via `rule_store_load_unlocked` (NVS-only — missed any uncommitted
  writeback-overlay edit), flipped `enabled`, and wrote via
  `rule_store_save_unlocked` (direct NVS, bypassing the overlay), so the next
  flush of a still-pending overlay WRITE for the same id would **overwrite the
  enable/disable with the stale copy** (silent lost edit); it also lacked the
  `if (!s_nvs)` init guard every other public fn has, so a pre-init call could
  `xSemaphoreTake(nullptr)` and crash. Confirmed callerless in production (only
  the IDF test referenced it). Routing it through the overlay would add a racy
  read-modify-write API nobody calls, so the function was deleted (trimming the
  public surface ahead of open-sourcing); callers flip `enabled` via the
  existing overlay-aware `rule_store_load` + `rule_store_save`. Test
  `enabled flag persists` re-expressed through that supported path.
- **rule_store** (LOW, FINDINGS §7, `rule_store.cpp` `rule_store_load_all`):
  closed an overlay-merge vanish window. `load_all` released `s_mutex` after
  the NVS walk and only THEN merged the writeback overlay (which takes the
  separate `s_mtx`); because the reader reads NVS-then-overlay in the same
  order the flusher writes NVS-then-clears-overlay, a slot flushed between the
  two reads could be absent from BOTH and transiently vanish from that one
  snapshot. The `s_mutex` release now happens AFTER `rule_store_foreach_dirty`,
  so the merge runs while `s_mutex` is held: the flusher's `rule_store_save` /
  `rule_store_delete_err` then blocks on `s_mutex` and cannot commit-then-clear
  mid-merge (the slot stays WRITE in the overlay and is picked up). Deadlock-free
  — `load_all` nests `s_mutex ⊃ s_mtx` and the flusher never holds `s_mtx`
  while taking `s_mutex` (it releases `s_mtx` before the NVS write), so there is
  no opposite-order nesting; `merge_cb` dedups by `rule_id`, collapsing any
  transient NVS+overlay duplicate.

### Fixed — Medium (P4 findings batch, T27 device_shadow)

- **device_shadow** (MED, FINDINGS §9, `device_shadow.cpp` config-blob path):
  `set_config` stored a caller-supplied `DeviceConfig` and `nvs_load_config`
  loaded the raw NVS blob with **no integrity check and no bounds clamp** — a
  `filtered_count` / `debounce_ignore_count` > `DEVICE_CONFIG_FILTER_MAX` (8)
  made `shadow_pipeline_filter` and the debounce-bypass scan read past the
  8-slot `filtered[]` / `debounce_ignore[]` arrays. Both counts are now
  **clamped to `DEVICE_CONFIG_FILTER_MAX` on every ingest path** (caller
  `set_config` + `nvs_load_config`), and the config 'c' blob is now persisted
  as `[CfgBlobHdr][DeviceConfig]` with a version + CRC32 header — mirroring the
  F26 attr-blob defence — so a torn/corrupt/hand-poked config is **rejected on
  load** (entry falls back to defaults + re-seeds from the SPA) instead of
  being trusted. `NVS_SHADOW_VERSION` bumped 7→8 (headerless v7 'c' blobs are
  wiped on first boot, not rejected per-device).
- **device_shadow** (MED, FINDINGS §9 / SHA-F3, `device_shadow.cpp` table +
  NVS leak): shadow entries were **never freed** — there was no remove API and
  REST/HAP setters `find_or_create` arbitrary ieee, so departed / mistyped
  devices permanently consumed one of the `ZAP_MAX_DEVICES` (200) table slots
  **and** left their per-device debounce/occupancy `xTimer`s scheduled (a
  device-churn workload eventually exhausts the FreeRTOS timer pool and breaks
  `xTimerCreate` for legitimate joins); `clear_attrs` additionally erased only
  the 'a' attr-blob NVS key, leaking the 'c' config blob in flash forever. New
  **`device_shadow_remove(ieee)`** reclaims the slot (swap-with-last + timer-ID
  fix-up on the relocated entry), deletes both timers, and erases **both** the
  'a' and 'c' NVS keys. Honours the T10 LEAF lock model: the slot/timer work is
  under `s_mutex` (0-tick non-blocking timer deletes), the NVS erases run AFTER
  the lock is released, and no `event_bus` publish is performed. Wired into the
  P4 HAP **hard** DEVICE_DELETE ("forget forever") path in `hap_dispatch.cpp`
  (replacing the leaky `clear_attrs`), NOT the soft DEVICE_LEAVE — soft-leave
  intentionally keeps slot + NVS so a rejoin restores state without a fresh
  interview.
- **device_shadow** (MED, FINDINGS §9, `device_shadow.cpp` `upsert_cache`):
  when a device exceeded `SHADOW_ATTR_MAX` (32) distinct attrs, new keys were
  **silently dropped** (multi-gang switches / climate TRVs that expose many DPs
  lose late-reporting attrs). Now logs **once per device** (gated by a new
  `attr_overflow_logged` flag, re-armed by `clear_attrs` / `device_shadow_remove`)
  so the condition is diagnosable without per-frame spam on the radio RX path.
  Deliberately **no auto-eviction**: the only obvious eviction target
  (`_`-prefixed diagnostics) includes internal bookkeeping like `_last_seen`
  that the occupancy/last-seen logic depends on, so silent eviction would trade
  a visible dropped-attr for a subtle correctness bug. The 32-slot cap is now a
  documented limit (raising it is a schema bump, out of scope).

### Fixed — Medium (P4 findings batch, T28 metrics / zap_common)

- **metrics** (MED, FINDINGS §8, `metrics_export_mqtt.cpp` snapshot formatter):
  when the JSON snapshot did not fit the caller's buffer, the formatter
  truncated mid-document (dropping the closing `}}` and possibly a value
  mid-token) but **returned the truncated length anyway** — the `truncated`
  flag was computed and discarded. The MQTT publisher then shipped that
  **syntactically invalid JSON** to the broker, poisoning every subscriber's
  parser. `mqtt_format_snapshot_json` now **returns 0 when the writer
  truncated**, so the caller skips the publish entirely rather than emit
  garbage; the buffer is still left NUL-terminated. (Prometheus text stays
  valid line-wise under truncation, so its `return off` contract is unchanged.)
- **zap_common** (MED, FINDINGS §8, `sys_metrics.h` CPU% sampler): the
  per-core CPU%-baseline state was held in function-local `static` vars inside
  a `static inline` **header** function. Those statics are **per-translation-
  unit, not "per-call-site"** as the old doc claimed — two call sites compiled
  into one TU, or two tasks racing one TU's copy, shared and corrupted a single
  rolling window with **no synchronisation**, reporting bogus CPU%. The
  baseline now lives in a **caller-owned `sys_metrics_cpu_ctx_t`** passed by
  reference, so each measurement cadence owns its own window. Signature changed
  to `sys_metrics_sample_cpu_pct(sys_metrics_cpu_ctx_t&, uint8_t&, uint8_t&)`;
  both firmware callers updated (P4 heartbeat `hap_dispatch.cpp`, S3
  `/api/status` `api_system.cpp`), each with a private file-scope ctx touched
  by a single task. A `seeded` flag keeps the first sample at 0 (no garbage
  delta) and distinguishes a genuine zero baseline from "never sampled".
- **zap_common** (MED/SMELL, FINDINGS §8, `zcl_attribute.h` set helpers):
  `zcl_attr_set_int` / `zcl_attr_set_str` called `std::strncpy(dst, key, n)`
  with an **unguarded `key`** — `strncpy` from `NULL` is UB. The null guard
  existed only inside the optional `ZCL_ATTR_ASSERT_KEY_FITS` macro (and
  `set_str` guarded `val` but not `key`). Both helpers now **substitute `""`
  for a null `key` (and `val`)** before any copy, so a null name on the decode
  path yields a well-defined empty-keyed attribute instead of a crash.
- **metrics** (LOW/DUP, FINDINGS §8, `metrics_export_mqtt.cpp:31`): the
  `Writer` struct + `wput` bounded-format helper was duplicated **verbatim** in
  the Prometheus exporter and re-implemented as the `append` lambda in
  `metrics.cpp`'s text dump. Hoisted to **one** `metrics::detail::Writer` /
  `detail::wput` in the component-private `metrics_internal.h`; all three
  formatters now share it (the dump lambda forwards to `detail::wput`). The
  shared `wput` carries a `printf` format attribute so -Wformat coverage
  survives the hoist.

#### Tests
- **metrics** on-target (`test/main/test_metrics.cpp`): the MQTT truncation
  case now asserts the formatter **returns 0** (skip-publish contract) in
  addition to NUL-termination, plus a positive case asserting an untruncated
  snapshot returns >0 and closes its braces.
- **zap_common** host (`test/host/`): added `test_sys_metrics.cpp` (proves two
  contexts on interleaved cadences do **not** cross-corrupt baselines, first
  sample is 0, per-core independence — drives scripted FreeRTOS counters via
  new `stubs/freertos/*` + `stubs/sdkconfig.h`) and `test_zcl_attribute.cpp`
  (null key, null val, both-null → no UB; plus normal population + over-long
  key truncation). Both wired into the host ctest suite (now 3 executables).

### Fixed — Medium (P4 findings batch, T25 zigbee_mgr / znp_driver radio stack)

- **zigbee_mgr / zhc_send_bridge** (MED, FINDINGS §4, `zcl_commands.cpp:27`
  DUP): the AF_DATA_REQUEST header assembly + SRSP status check was
  copy-pasted across ~12 call-sites (on/off, level, color-temp, gen-time,
  default-resp, read, write, configure-report, cluster-command, magic
  packet, miboxer set-zones / tuya-setup) and in `zhc_send_bridge.cpp`, each
  with drifting retry params (timeouts 1000/2000/3000, attempts 1/2/3).
  Folded into ONE `af_data_request()` builder (+ exported `zigbee_af_send_zcl`
  for the adapter bridge); each call-site's original timeout/attempt count is
  **preserved** via parameters, not homogenised.
- **zigbee_mgr / zhc_send_bridge** (MED, FINDINGS §4, `zcl_commands.cpp:45`
  SMELL→correctness): AF_DATA_REQUEST was blind-retried up to 3× with the
  SAME trans_id after an SRSP loss — the SRSP only confirms the NCP
  *accepted* the frame, so if attempt 1 reached the NCP but its SRSP was
  dropped, the command executed twice on-air (a Toggle visibly double-fires,
  a level/color step overshoots). Non-idempotent commands (On/Off, Level,
  ColorTemp, cluster-specific device commands, the whole zhc_send_bridge
  adapter→radio path) now send EXACTLY ONCE and gate success on the
  asynchronous AF_DATA_CONFIRM via the existing `znp_confirm` ring — no
  blind re-send. Idempotent frames (attribute reads, absolute-value writes,
  configure-reporting, gen-time / magic-packet probes, default-resp) keep
  their multi-attempt SREQ retry since re-sending them is harmless.
- **zigbee_mgr** (MED, FINDINGS §4, `zcl_seq.cpp:17`): the TSN counter
  `fetch_add` wrapped 0xFF→0x00, handing every 256th frame the reserved
  "not-set" TSN 0 (which the send bridge writes as a placeholder and the
  confirm ring treats as a live key) → mis-correlation. Now a CAS loop
  that skips 0, so `zcl_seq_next()` always returns 1..255.
- **znp_driver** (MED, FINDINGS §4, `znp_rx.cpp:76`): the AREQ dedup scope
  named `0xA1` as "TC_DEV_IND", but `0xA1` is ZDO_BIND_RSP — TC_DEV_IND is
  `0xCA`. So the rapid device-announce double-bursts the dedup was meant to
  catch (Xiaomi devices fire two on wake) were never deduped, while two
  distinct BIND_RSPs within 200 ms were wrongly dropped (losing a bind
  outcome). Target corrected to `0xCA` (+ comment).
- **znp_driver** (MED, FINDINGS §4, `znp_driver_shim.cpp:30` DUP + `:85`
  BLOCK): the legacy `mt_fcs` / `mt_encode` reimplemented the MT framing
  already owned by `znp_parser.cpp` (two paths that could drift) → they now
  delegate to the parser's single `znp_mt_fcs` / `znp_mt_encode`. And
  `znp_sreq_retry` looped with NO backoff and retried even
  RESET_DURING_CALL, hammering a resetting NCP 3× immediately and bypassing
  the F43 backoff → it now routes through `znp_call_retry` (25/50/100/200 ms
  backoff, fail-fast on TRANSPORT_DOWN/INTERNAL_ERROR).
- **znp_driver** (MED, FINDINGS §4, `znp_worker.cpp:289` BLOCK): `znp_call`
  applied the FULL `timeout_ms` to each of acquire-slot, queue-send, and the
  worker's SRSP wait (≈3× the advertised timeout under contention) before
  blocking `portMAX_DELAY` on the slot sem. A single deadline now budgets
  all the pre-send waits; the worker's SRSP wait is the time LEFT after the
  request is queued (floored at 50 ms), so a whole call is bounded by
  ~`timeout_ms`.
- **zigbee_mgr** (MED/SEC, FINDINGS §4, `zigbee_interview.cpp:311`):
  ACTIVE_EP_RSP set `endpoint_count` from the device's claimed count while
  the endpoint memcpy clamped separately — a truncated frame left
  uninitialised stack bytes reported as real endpoints (then probed +
  registered). The count is now clamped to bytes-present (and to 8) BEFORE
  it is consumed.
- **zigbee_mgr** (MED/SEC, FINDINGS §4, `zigbee_interview.cpp:375`): the
  SIMPLE_DESC out-cluster-list offset `out_off = 13 + in_cnt*2` was computed
  as `uint8_t` and wrapped for `in_cnt ≥ 122` (attacker-influenced wire
  data), parsing the out-cluster list from the wrong offset. Now size_t math
  with an explicit bound against the frame length.
- **zigbee_mgr** (MED, FINDINGS §4, `zigbee_interview.cpp:96`): a duplicate
  matching ZDO AREQ delivered into `store_rsp` (ZNP RX task) could memcpy
  into `s_rsp_buf` WHILE the interview task (other core) read the previous
  response — a torn cross-core read. `store_rsp` now CLAIMs each wait
  single-shot via a CAS on the cmd1 filter, so exactly one writer touches
  the buffer per arm.
- **zigbee_mgr** (MED, FINDINGS §4, `zigbee_interview.cpp:251`): a NODE_DESC
  failure makes `do_interview` return early before the work-copy's FAILED
  state is committed, leaving the live slot at NONE — the FAILED-keyed
  opportunistic re-interview never fired and the device was stranded until
  rejoin. The live slot is now stamped FAILED after the final attempt.
- **zigbee_mgr** (MED, FINDINGS §4, `zigbee_interview.cpp:662` BLOCK):
  `on_tc_dev_ind` runs in the ZNP UART RX task and blocked up to 100 ms in
  `xQueueSend` on a full join queue — a join storm stalled byte intake →
  UART overrun. Now a zero-timeout send with drop-oldest (the pool is
  already seeded/refreshed and the interview task re-reads the pool nwk).
- **zigbee_mgr** (MED, FINDINGS §4, `zigbee_mgr.cpp:99`): `s_init_done` was
  a plain bool written by `coordinator_start` and read by `on_reset_ind` on
  the RX task (its neighbours were already `std::atomic` for exactly this,
  ZB-F4) → `std::atomic<bool>`.
- **zigbee_mgr** (MED/LEAK, FINDINGS §4, `zigbee_mgr.cpp:1021`):
  `zigbee_mgr_init` was not idempotent — a retry after a `coordinator_start`
  failure re-created `s_zcl_queue` + spawned a second `zcl_attr_task` (old
  queue leaked, duplicate consumer + duplicate AREQ subscriptions). The
  one-time wiring (queue, tasks, sub-module inits, AREQ registrations) is now
  guarded by a null-check on the queue.
- **zigbee_mgr** (MED/SEC, FINDINGS §4, `zigbee_mgr.cpp:405`): the PRECFGKEY
  (network key) NV write was hex-dumped in plaintext by the wire trace
  (`znp_worker.cpp` TX, `znp_rx.cpp` RX) — a debug capture would contain the
  live key. A new `znp_wire_is_sensitive()` flags SYS NV writes of key item
  ids (PRECFGKEY 0x0062, NWK active/altern key-info 0x003A/0x003B); both
  trace sites now print the header but REDACT the payload.
- **zigbee_mgr** (LOW/SEC, FINDINGS §4, `zigbee_mgr.cpp:342`): the generated
  `net_key[16]` stack buffer was never zeroized after commissioning →
  `explicit_bzero` immediately after its last use (the PRECFGKEY write).

### Fixed — High/Medium (P2 findings review, T19 mqtt_gw / tg_gw lifecycle & injection)

- **mqtt_gw (S3)** (HIGH, FINDINGS §7, `mqtt_gw_s3.cpp:53`): the `s_client`
  stop/destroy/recreate ran UNSYNCHRONIZED from REST handlers, the esp_timer
  re-arm/deferred-start callbacks, the STA-up handler, and the pub-worker's
  deferred-disable while the worker could be inside `esp_mqtt_client_publish`
  → use-after-free of the client handle. A new `s_client_mtx` (created in
  `mqtt_gw_init` at single-threaded boot) now fences EVERY
  `esp_mqtt_client_*` lifecycle call (init/start/stop/destroy/subscribe/
  unsubscribe) AND the worker's publish; the worker snapshots the handle
  under the lock and publishes under it. Publish-under-lock chosen over a
  snapshot-revalidate handshake: the worker is the sole publisher (already
  serialized by the queue), so the only task that can stall behind a blocking
  QoS>0 publish is a rare concurrent config write — acceptable for a gateway
  and far simpler/safer than reopening a destroy-vs-use window. The
  esp-mqtt **event-handler** subscribe/publish (CONNECTED) stay unlocked by
  design (they run on the mqtt task, which cannot destroy its own client and
  must not block on a mutex a lifecycle op could hold).
- **mqtt_gw (S3)** (MED, `:172`): `MQTT_EVENT_DATA` ignored fragmentation —
  payloads > the 4608 B buffer were forwarded to `rx_cb` truncated to the
  first chunk with no indication. Now drops any event where
  `data_len != total_data_len` and logs a one-shot warning; continuation
  chunks (`topic_len==0`) remain dropped by the existing guard. Reassembly
  is intentionally not added (the inbound command contract is sub-buffer).
- **mqtt_gw (S3)** (MED, `:419`): a rule/Lua publish topic was not sanitized
  for MQTT wildcards (`+`/`#`) or control chars — one wildcard PUBLISH makes a
  spec-compliant broker drop the connection (reconnect churn). New
  `mqtt_topic_ok()` rejects `+`, `#`, NUL/control/DEL bytes, and overlong
  names; the prefix-format overflow path (was silently publishing to the
  UNPREFIXED topic) now DROPS instead. Host-proven.
- **mqtt_gw (S3)** (MED, `:112`, `:306`, `:335`): the broker URL (sole carrier
  of `mqtt://user:pass@host` creds) was logged verbatim at INFO on every
  (re)start. New `redact_userinfo()` prints `scheme://host:port` only and is
  applied at all three log sites. Host-proven.
- **mqtt_gw (S3)** (LOW, `:407`): re-subscribe overwrote the single stored
  filter without unsubscribing the previous one (old subscription kept
  delivering until reconnect). Now unsubscribes the prior live filter before
  replacing it.
- **mqtt_gw (P4)** (MED, `mqtt_gw_p4.cpp:40`): the publish-path serialization
  mutex was lazily created (`if(!s_mutex)…`) in the hot path — two first
  callers each created one (one leaked, serialization broken). Created once in
  `mqtt_gw_init` at single-threaded boot.
- **tg_gw (S3)** (HIGH, FINDINGS §7, `tg_gw_s3.cpp:196`): `chat_id` and
  `parse_mode` (HAP/NVS-sourced, reachable from Lua via `tg_gw_setchat`/`send`)
  were interpolated UNESCAPED into the Telegram API JSON body — a `"` injected
  arbitrary request fields. Both are now run through `json_escape`, and
  `parse_mode` is whitelisted to {`Markdown`,`MarkdownV2`,`HTML`} (any other
  value is rejected and the send dropped). Host-proven.
- **tg_gw (S3/P4)** (MED, `tg_gw_s3.cpp:29` / `tg_gw_p4.cpp:29`): token-length
  cap drift — S3 rejected >80, P4 accepted up to 96, so an 81–96-char token was
  forwarded by P4 then silently dropped on S3. A single `TG_TOKEN_MAX` (80,
  comfortably over the ~46-char real token and within the 96-byte HAP field)
  now lives in `tg_gw.h` and is used by both cores.
- **tg_gw (S3)** (LOW, `tg_gw_s3.cpp:127`): `json_escape` truncated byte-wise
  and could split a UTF-8 multibyte sequence → invalid JSON the Telegram API
  rejects (HTTP 400). On truncation it now backs off to the previous UTF-8
  code-point boundary. Host-proven.
- **tg_gw (P4)** (MED, `tg_gw_p4.cpp:95`/`:76`): the static ~3.2 KB pack buffer
  (and the ~3.2 KB `HapTgSend`) were used WITHOUT a mutex — concurrent
  `tg_gw_send` from two tasks (rules + Lua) could corrupt the frame
  mid-pack/send, and a stack-local `HapTgSend{}` per call was a 3.2 KB stack
  hit. Both are now `static`, serialized by an init-created `s_send_mutex`
  (same pattern as `mqtt_gw_p4`).
- Added committed host tests (`mqtt_gw/test/host/` + `tg_gw/test/host/`)
  pinning the four pure security helpers — `mqtt_topic_ok`, `redact_userinfo`,
  `json_escape` (incl. the UTF-8 truncation back-off and the chat-id
  JSON-framing injection case) and the `parse_mode` whitelist — as a
  regression guard (mirror-source pattern, per the `zap_store/test/host`
  precedent). Also reworded stale `mqtt_gw_s3.cpp` comments that referenced a
  deleted `restart_client()` wrapper.

### Fixed — High/Medium (P2 findings review, T18 simple_rules / cron integrity)

- **simple_rules / rule_store** (HIGH, FINDINGS §7, `simple_rules.cpp:84`):
  `next_rule_id()` scanned only the 64-entry in-memory cache for the max id,
  but `rule_store` persists up to 256 NVS slots — a persisted-but-uncached
  rule's id was reissued and the deferred `rule_store_mark_dirty` silently
  overwrote the original (permanent data loss). New `rule_store_max_id()`
  walks every `r_%04X` NVS key + the writeback overlay (no blob loads);
  `next_rule_id` derives `max(store, cache) + 1`, and the wraparound free-id
  scan probes the store too. Host-proven (65 persisted, empty cache → next id
  = 66, never 1).
- **dsl_parser** (HIGH, FINDINGS §7, `dsl_parser.cpp:206`): an unclamped
  `strtod`→`int32_t` cast was UB for out-of-range literals (e.g. a
  REST/cloud-supplied `#temp>1e20`). Now checks `errno==ERANGE` and the
  rounded magnitude against the int32 bounds before the cast; `1e20`,
  `-1e20`, `99999999999`, and garbage all return `ERR_BAD_TRIGGER`, while
  `2147483647` (INT32_MAX) still round-trips. Follow-up: the magnitude-only
  guard let `nan` slip through (`nan>=X` / `nan<=Y` are both false) into
  `(int32_t)nan` = UB; a `!std::isfinite(d)` check now rejects `nan`/`-nan`/
  `inf`/`-inf` too (confirmed via UBSan `float-cast-overflow`).
- **simple_rules** (MED, FINDINGS §7, `:580`): a full active-rule cache used
  to persist to NVS yet return `true`, so the rule was accepted but never
  evaluated (silent no-op). `add` now returns `false` and sets
  `dsl_last_error()` = "rule cache full (max 64 active rules)"; the HAP
  `RULE_CREATE` handler already relays `dsl_last_error()` to the SPA/cloud, so
  no net-core change was needed.
- **simple_rules** (MED, FINDINGS §7, `:101`): `reload_locked` loaded only the
  first 64 NVS slots and silently dropped the rest. The 64-active cap is kept
  intentionally (raising to 256 = +~130 KB P4 DRAM); a new `rule_store_count()`
  drives an `ESP_LOGW` with the count of persisted-but-inert rules, and the
  64-active limit is documented in `simple_rules/README.md`.
- **simple_rules / dsl_parser** (MED, FINDINGS §7, `:574` + `dsl_parser.cpp:336`):
  a DSL ≥ 500 B parsed in full for the live rule but persisted truncated to
  499 (`slot.src`), so the rule mutated/failed-parse after reboot. `add`/`update`
  now reject `dsl_len >= sizeof(slot.src)` with "rule DSL too long (max 499
  bytes)"; an action-section overrun returns the new `ERR_ACTION_TOO_LONG`
  instead of a silent clamp. No truncate-persist path remains.
- **cron_parser** (MED, FINDINGS §7, `cron_parser.cpp:95`/`:54`): an expression
  or per-field buffer ≥ 128 chars was silently `strncpy`-truncated and could
  still parse into a WRONG schedule. Both now reject (`strlen >= sizeof(buf)`).
- **cron_parser** (MED, FINDINGS §7, `cron_parser.cpp:260`): `cron_next` did
  `t += 60 - tm_sec`, which is `+0` when `tm_sec == 60` (leap second; reachable
  on glibc host tests) → infinite loop. `tm_sec` is now clamped to 59 before
  the jump, matching `cron_matches`. New host suite under
  `cron_parser/test/host/`.
- **device_shadow / simple_rules** (MED, FINDINGS §7, `simple_rules.cpp:297`):
  the `ZIGBEE_TOGGLE` action put a `ShadowAttr[32]` (~2.7 KB) + a 522 B device
  snapshot on the shared event-drain task stack just to read one attr by key.
  New `device_shadow_get_attr(ieee, key, out)` (LEAF s_mutex only — no NVS,
  publish, or nested lock, per T10) returns the single slot; the toggle stack
  arrays are dropped.

### Fixed — Medium (P2 findings review, T14 HAP correlation edges)

- **hap_session**: enlarged the receive-side dedup `SEEN_RING` 16→64 entries
  and added a wrap-aware monotonic high-water fast-path. The 16-entry ring
  evicted a NEEDS_ACK frame's `(seq,type)` after 16 intervening distinct
  frames, so a burst plus one lost ACK re-dispatched the original on
  retransmit (double DEVICE_DELETE / double rule-create). The ring now catches
  recent exact dups; the high-water mark (`seq_diff(high_water, seq) >=
  WIN_SIZE`, uint16 wrap-safe via the signed-difference trick) catches
  burst-evicted dups. Both drop-without-dispatch but still re-ACK so the peer
  stops retransmitting. (FINDINGS §1.3, :252)
- **hap_session**: session clock `now_ms()` / `WinSlot.sent_ms` moved from a
  uint32 `xTaskGetTickCount()*portTICK_PERIOD_MS` (wrapped at ~49.7 days, and
  its `sent_ms==0` sentinel collided with a real post-wrap tick of 0 → one
  spurious retransmit per slot per wrap) to `int64_t esp_timer_get_time()/1000`.
  No magic sentinel — the per-slot `active` flag is the armed flag (the tick
  loop short-circuits inactive slots before reading `sent_ms`). The
  retransmit-timeout delta `ms - sent_ms < ACK_TIMEOUT_MS` stays exact under
  int64 (both monotonic ms-from-boot; small positive delta). Added `esp_timer`
  to the component's PRIV_REQUIRES. (FINDINGS §1.4, :68)
- **hap_session**: `hap_session_next_seq()` called before init returned 0,
  which the wire treats as "no correlation" — a roundtrip waiting on that seq
  silently timed out. It now returns the explicit `SEQ_SENTINEL_UNINIT` (0),
  and `hap_session_send()` (the shared send path used by BOTH P4 and S3
  transports) hard-rejects any frame carrying it with an `ESP_LOGE` and
  `return false`, so a pre-init send fails fast at the call site instead of
  eating a full timeout. (FINDINGS §1.5, :143)

### Fixed — Medium (P2 findings review, T17 zap_store capacity + delete robustness)

- **zap_store**: `zap_store_save_device` had no capacity check. At
  `count == ZAP_MAX_DEVICES` (200) a new IEEE took `idx = count`, bumped `cnt`
  past 200, and wrote an unreferenced 522 B blob; every later save of that
  IEEE re-missed the in-RAM index and re-appended another blob at an
  ever-higher idx — unbounded NVS growth until partition exhaustion. Now a new
  device at a full store is rejected with `ESP_LOGE` and `return false` BEFORE
  any blob write or `cnt` bump; an existing device is always an in-place
  rewrite and is never rejected. (The in-RAM `pool_add` already guards at 200;
  this is the persistence-layer safety net.) (FINDINGS §8.1, :150)
- **zap_store**: `zap_store_delete_device` always allocated
  `ZAP_MAX_DEVICES × 522 B` (~102 KB) regardless of the actual device count,
  so delete could fail on a fragmented heap with PSRAM unavailable. It now
  reads `cnt` under the store lock first and sizes the scratch buffer to the
  actual count (still PSRAM-preferred with internal-heap fallback); an empty
  store short-circuits with no allocation. (FINDINGS §8.2, :208)
- **zap_store**: `zap_store_load_devices` took no `store_lock` (racing
  save/delete once the flush task runs) and never null-checked `pool`. It now
  validates `pool`/`max_count` and holds the store lock across the multi-blob
  scan. Lock order is safe: `s_store_mutex` is the innermost lock — the flush
  layer always releases its own `s_mtx` before calling save/delete, so this
  cannot invert against it. (FINDINGS §8.3, :292)
- **zap_store**: `zap_device_crc` stack-copied the 522 B struct even though the
  save path already holds a crc-zeroed copy. Split into `zap_device_crc_zeroed`
  (no copy — save path CRCs its already-zeroed copy in place) and the existing
  `zap_device_crc` (load path, which must preserve the stored crc32 for the
  equality check). Same CRC bytes; one fewer 522 B stack frame + memcpy per
  save. (FINDINGS §8.4, :74)
- **zap_store**: the NVS namespace now references `zap_nvs::DEVICE_POOL` from
  the central registry instead of an inlined `"zap_v0"` literal. (FINDINGS §8.5, :17)
- Added a host test (`test/host/test_zap_store_logic.cpp`) covering the pure
  capacity-reject decision table and the CRC-in-place ⇔ CRC-with-copy
  equivalence against the real `ZapDevice` layout (plain cmake, `-Werror`).

### Changed — DRAM→PSRAM static buffer sweep (P1, T12)

- **device_shadow**: the four 32-slot `ZclAttribute` pipeline staging arrays
  (`filtered`/`bypass`/`merge`/`out`, 4 × 2,688 B ≈ 10.7 KB) moved to PSRAM
  via `EXT_RAM_BSS_ATTR` — warm path (mutex-serialised per-report staging),
  never ISR/DMA.
- **zhc_adapter**: `g_merged` (8192-entry merged-registry pointer array,
  32 KB) was commented as "PSRAM-resident" but actually sat in internal
  `.bss` — now genuinely placed in `.ext_ram.bss`; the `= {}` initialiser
  dropped (startup zeroes the section, same semantics).
- **tg_gw (S3 side)**: Telegram send staging `body`/`text_esc` (6.7 KB) to
  PSRAM — cold worker-task path.
- Net effect (`idf.py size`): P4 `.bss` 139,300 → 95,780 B (−43,520 B =
  device_shadow 10,752 + g_merged 32,768, exact). The tg_gw share lands in
  the S3 app's −35 KB sweep total. `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
  verified in BOTH firmware sdkconfigs, so the attribute is effective on P4
  as well as S3.

### Added

- **zhc_adapter / zigbee_mgr**: wire the join-time `configure_write` hook so
  `ConfigStepOp::Write` steps actually transmit. embedded-zhc's runtime already
  invokes `ctx.configure_write` during configure, but nothing registered a
  transport, so it stayed null and Write steps were inert no-ops. Added
  `zhac_configure_write_fn_t` + a dedicated `zhac_adapter_register_configure_write`
  setter (separate from `..._register_configure_ex` so the existing caller's
  signature is untouched), a `configure_write_bridge` forwarding through the
  addr-mutex `g_cfg_ieee`/`g_cfg_nwk` globals exactly like `configure_read_bridge`
  (wired at both ctx-setup sites — configure path + inbound-decode path), and the
  `zhc_cfg_write_af` bridge backing it with `zigbee_zcl_write_attr`.
  `zigbee_zcl_write_attr` gained an optional trailing `manufacturer_code`
  parameter (default 0): non-zero builds a manufacturer-specific ZCL frame
  (FC=0x04, 5-byte header), mirroring `zigbee_zcl_read`'s manu branch exactly;
  the bridge forwards `manu` end-to-end. This makes lumi 0xFCC0 writes actually
  work — z2m emits them as `endpoint.write("manuSpecificLumi", {...},
  {manufacturerCode: 0x115f})` and Aqara hardware rejects a profile-wide write
  (the earlier "0xFCC0 is fine profile-wide / cluster id carries the
  specificity" rationale was wrong — the lib's own def/test thread
  `manu_code=0x115F` through to the transport). The default-0 param leaves
  every existing profile-wide caller (Tuya 0x8004, IAS CIE-address write)
  byte-for-byte unchanged. **NEEDS HARDWARE TEST** — host build cannot
  exercise the ZNP radio path.
- **device_shadow / hap_json**: per-device `throttle_ms` report rate-limit knob,
  carried over `DEVICE_OPTIONS_SET` and applied by the existing
  `shadow_pipeline_throttle_pass` (new `device_shadow_set_throttle_ms`, NVS-
  persisted). Caps the message flood from chatty Tuya-DP sensors (air-quality
  monitors) that report every few seconds with no device-side reporting-interval
  control. `hap_json_{encode,decode}_device_options_set` gained an optional
  `throttle_ms` field. (#84)

### Changed

- **zhc_adapter**: benign protocol housekeeping frames now log at debug as
  "(protocol frame, no state)" instead of the INFO "(no match)" line that
  read like a device-coverage gap — any ZCL Default Response (global cmd
  0x0B) and Tuya 0xEF00 MCU management (0x10/0x11 version, 0x24 sync-time,
  0x25 gateway-connection-status). Decode/shadow behaviour unchanged.

### Fixed — Critical

- **simple_rules / event_bus**: a self-feeding rule (`ON Event#x DO event x
  ENDON`) wedged the P4 main loop forever — the `event` action re-enqueued
  into the same queue `event_bus_drain` was draining with ticks=0, so the
  drain never returned, the watchdog rebooted, and the NVS-persisted rule
  re-wedged every boot. The old `MAX_DISPATCH_DEPTH` counter was dead code
  (delivery is queue-based; every hop is a fresh dispatch at depth 0) and is
  removed. `RuleEventPayload` now carries a `hop` TTL (`name` 96→95 B,
  external producers stay hop 0 via zero-init); the `event` action refuses
  to republish past `MAX_EVENT_HOPS` (8) and logs the offending rule id.
  New host test harness `simple_rules/test/host/` reproduces the wedge and
  verifies the cut. (simple_rules.cpp:411)
- **zhc_adapter**: the fallback pool (synthetic defs for unmatched devices)
  rebuilt a slot's `PreparedDefinition` + expose/fz/binding arrays IN PLACE on
  every `synth_definition()` call with no lock — while the radio task
  dispatched frames through the same pointers and httpd paths
  (`build_exposes_json` / `has_def`) triggered concurrent rebuilds: torn
  counts / nulled expose pointers in normal operation for any fallback
  device. The pool is now serialized by a FreeRTOS mutex (created from a
  global ctor, same pattern as `g_cfg_addr_mtx`), and a published def is
  never rewritten: each slot keeps an A/B double buffer — rebuilds (now
  gated on `built`/base/label change instead of unconditional) write the
  inactive half and publish by flipping the active index, so readers holding
  the old pointer keep seeing consistent bytes for one more generation
  (contract documented in `zhc_adapter_fallback.hpp`). LRU eviction (and
  `clear`) now notify a new internal hook
  `zhc_adapter_internal::invalidate_cached_defs_in(begin,end)` that clears
  any `IeeeSlot.cached_def`/`cached_supplement` pointing into the victim
  slot's storage before it is repurposed — previously the evicted device's
  frames silently kept decoding through the NEW device's def (cross-device
  state corruption). Eviction also picks a real victim now: `last_used_ms`
  is stamped from `esp_timer` on every touch instead of the hardcoded
  `now_ms=0` that always victimized slot 0. Costs ~25 KB extra .bss for the
  16-slot A/B halves. (zhc_adapter_fallback.cpp:455,92,406) **NEEDS
  HARDWARE TEST** — component is IDF-only; verified by host syntax check +
  reader/writer trace, full build gated on the IDF toolchain.
  Review follow-up: cache invalidation centralized in `reset_slot()` (now
  also covers the empty-slot realloc path), the decode-miss re-cache race
  closed via an ownership re-check under the pool mutex
  (`zhc_fallback::owns`), and `g_pool` moved to PSRAM (`EXT_RAM_BSS_ATTR`,
  freeing the full ~48 KB — including the pre-existing ~23 KB — of internal
  .bss).
- **zigbee_mgr**: hold pool mutex across the `on_tc_dev_ind` find-then-mutate
  sequence so a concurrent `pool_remove` (swap-with-last from user delete or
  ZDO_LEAVE_IND) can no longer relocate the entry between lookup and write,
  preventing corruption of an unrelated device's `nwk_addr`. (F-01)
- **zigbee_mgr**: collapse the two-call `pool_find_by_ieee` lookup in
  `do_interview` into a single locked find-or-create, eliminating the TOCTOU
  window where a delete between calls left `is_rejoin=true` but a freshly
  added pool slot, suppressing the support_state reset and clearing the
  shadow attrs for the wrong device. (F-02)
- **zap_store**: bump the persisted `cnt` BEFORE writing a new-device blob
  in `zap_store_save_device`, so a power loss between the two writes leaves
  a referenced-but-empty slot (caught by the load CRC check) instead of a
  written-but-unreferenced blob that load_devices would never see; prevents
  permanent loss of friendly name / configure state on hard crash. (F-03)

### Fixed — Important

- **zigbee_mgr**: the ZCL Default-Response TX gate is now spec-correct — it
  no longer answers global frames that are themselves *responses* (Read /
  Write Attributes Response, Configure-Reporting / Read-Reporting-Config
  Response, the Discover-* responses, Write-Structured Response) or
  Write-Attributes-No-Response (0x05). Previously every unsolicited unicast
  frame was ACK'd, so the coordinator sent a Default Response back at its own
  interview Read-Attributes Responses. Cluster-specific commands and the
  unsolicited Report Attributes (0x0A) stay DR-eligible, so the sleepy-Tuya
  retransmit suppression is unchanged. New helper
  `zcl_global_cmd_wants_default_response`.
- **zigbee_mgr**: switch `s_expected_rsp_cmd1` / `s_expected_src_nwk` from
  `volatile` to `std::atomic` with release/acquire ordering, and arm the
  filter BEFORE flushing the response semaphore in `wait_rsp`. Closes the
  ~700 µs micro-window where ZDO AREQs were dropped by `store_rsp` because
  the previous wait's cleared filter (`0xFF`) hadn't been re-armed yet —
  was the cause of spurious 3 s interview timeouts under load. (F-04)
- **zap_store**: stack-snapshot the device in the `zap_store_mark_dirty`
  table-full fallback before releasing the flush mutex and calling
  `save_device`. Prevents the interview path (which passes a raw pool
  pointer) from racing with `pool_remove` and persisting the wrong device
  under the original IEEE. (F-05)
- **device_shadow**: split `device_shadow_init` into init-only
  (mutex/queue/alloc/task spawn) and a new
  `device_shadow_restore_from_pool(pool, count)` that walks an
  already-loaded pool slice. Boot orchestrator on P4 now calls
  `zigbee_pool_init` + `zigbee_pool_restore_persisted` BEFORE
  `device_shadow_init`, then drives the restore from the populated pool
  under the pool lock. Eliminates the second NVS namespace open + the
  84 KB scratch PSRAM allocation + the O(n²) `save_device` blob scans
  that ran on every boot — boot is now flat instead of ~4 s for a
  200-device fleet. `zigbee_mgr_init` no longer calls
  `zigbee_pool_init` / `zigbee_pool_restore_persisted` (responsibility
  moved up to the boot orchestrator). (F-06)
- **zhc_adapter**: guard the module-level `g_cfg_ieee` / `g_cfg_nwk`
  globals with a FreeRTOS mutex covering the configure call sequence, the
  ZCL dispatch sequence, and `set_runtime_addr`. Prevents the dual-core
  P4 race where a parallel `try_decode` rewrote the destination address
  mid-configure and routed `configure_cmd_bridge` ZCL frames to the
  wrong device. Pushing addressing into `RuntimeContext` is the proper
  fix but requires upstream library signature changes; deferred. (F-07)
- **zigbee_mgr**: remove `extern ZapDevice* s_pool` / `extern uint16_t
  s_pool_count` from the public `zigbee_pool.h`, give the storage static
  linkage in `zigbee_pool.cpp`, replace the inline accessors with
  out-of-line versions, and move the `zigbee_pool_remove` implementation
  from `zcl_commands.cpp` into `zigbee_pool.cpp` next to the storage.
  Contributors can no longer bypass the pool mutex by indexing the array
  directly. (F-08)
- **zigbee_mgr**: bump `CONFIGURE_QUEUE_DEPTH` from 16 to `ZAP_MAX_DEVICES`
  (200) and fall back to `schedule_retry(1 s)` on queue-full instead of
  silently dropping. Always-on routers (Tuya dimmers, Hue) that never
  rejoin no longer get stranded in `ConfigureState::PENDING` after a
  coordinator-restart storm. (F-09)

### Changed — Medium

- **zap_store**: drop the misleading "NVS v0" string from the init log
  (the schema version is the meaningful axis; the namespace name was being
  confused as a parallel version axis), and add a long-form migration
  contract to the `zap_store.h` public header documenting when to bump
  `ZAP_STORE_SCHEMA_VERSION` versus when to introduce a new NVS namespace.
  The "zap_v0" NVS namespace itself is intentionally NOT renamed — that
  would orphan existing deployments. Rename deferred. (F-10)

### Fixed — Critical (HAP stack review, 02-hap-stack.md)

- **hap_protocol**: rewrite the v3 wire-layout comment in
  `include/hap_protocol.h` for v4 and lock the preamble version byte to
  `HAP_VERSION` via `static_assert(HAP_PREAMBLE[3] == HAP_VERSION)`.
  Prior comment trailed the implementation by a whole version, so a
  third-party reader following the header to build a peer would fail
  `HAP_DECODE_BAD_VERSION` and `HAP_DECODE_BAD_HDR_CRC`. The static
  assert prevents bumping either constant without the other.
  (HAP F-03)

### Fixed — Important (HAP stack review, 02-hap-stack.md)

- **hap_session**: correct the public `hap_session_tick()` comment in
  `include/hap_session.h` — was "100 ms ACK timeouts, retransmits up to
  3x", impl is `ACK_TIMEOUT_MS = 1000` / `MAX_RETRIES = 5` (~5 s budget).
  Callers sizing response budgets from the header underestimated retry
  by 5×. (HAP F-04)
- **hap_protocol**: author a 400+ line spec-grade `README.md` covering
  the v4 wire format, two-stage SPI DMA framing, CRC algorithms and
  polynomials, the full HapMsgType registry, ACK/SEQ correlation
  semantics, sequence-number lifecycle, and SYNC / HEARTBEAT control
  flow. Prerequisite for fuzz harness and alternative-transport
  bindings. (HAP F-06)

### Changed — Medium (HAP stack review, 02-hap-stack.md)

- **hap_protocol**: add `HAP_DECODE_RESYNC = 7` and emit it from
  `hap_decode_stream` when the scanner locates a fresh PREAMBLE
  candidate after a bad head. Callers previously saw the original
  decode error (e.g. `HAP_DECODE_BAD_HDR_CRC`) with `*consumed = i`,
  which mis-attributes the failure in logs. Equivalent to a CRC error
  for retry purposes — `hap_decode_with_counters` recognises the new
  code without bumping a per-failure metric (the upstream error metric
  was already bumped on the first decode attempt; `METRIC_HAP_RESYNC_BYTES`
  still tracks the byte loss). Existing
  `test_hap_frame.cpp` "stream decode skips corrupted prefix" updated
  to assert `RESYNC` instead of the previous `BAD_MAGIC`. (HAP F-09)

### Fixed — Critical (net-core glue review, 04-net-core-glue.md)

- **rule_store**: add `bool* out_tombstoned` out-param to
  `rule_store_load_overlay` and check it in `rule_store_load` so a
  pending-but-not-yet-flushed tombstone short-circuits before the NVS
  fallthrough. Previously the overlay returned `false` for both
  "tombstoned" and "not found", letting deleted rules reappear from NVS
  for up to 5 s (flush-task period) and permanently after a power-cut
  before flush. (F-02)

### Fixed — Important (net-core glue review, 04-net-core-glue.md)

- **cron_parser**: rewrite DOM/DOW evaluation in `cron_matches` and
  `cron_next` as a 4-quadrant table — `*` on a field is now treated as
  the vacuous match it is per POSIX/Vixie. A pair like `0 8 1 * 1`
  now fires every 1st of the month OR every Monday at 08:00 (POSIX OR
  semantics), where it previously fired only on Mondays that also fell
  on the 1st. Added fixture tests for all four quadrants plus a
  `cron_next` assertion. (F-06)
- **mqtt_gw**: surface the silent drop path of `mqtt_gw_publish`
  through a new `METRIC_MQTT_DROPPED_MSGS` counter so operators can
  detect broker stalls and log-storm saturation. Drop path stays
  silent w.r.t. ESP_LOG (log-pipeline recursion is the reason it was
  silent in the first place), but the counter surfaces in `/metrics`.
  (F-07)
- **mqtt_gw**: default `CONFIG_MQTT_BROKER_URL` to empty string in the
  component Kconfig and refuse to start the client in `restart_client`
  when the runtime broker URL is empty. Previously the Kconfig
  fallback silently connected to whatever URL a developer had baked
  into their local sdkconfig — open-source distribution and
  supply-chain hazard. (F-09)
- **metrics**: register `METRIC_MQTT_DROPPED_MSGS` in the shared
  metric registry; needed by F-07.

### Fixed — High (P2 findings review, zhc_adapter slot table + decode locking)

- **zhc_adapter**: the per-IEEE slot table (`g_slots` / `RuntimeStore
  g_store`) was capped at 32 (`kMaxDevices`) while the network holds up
  to `ZAP_MAX_DEVICES` (200). Device 33+ aliased onto slot index 0 —
  permanently sharing device 0's `RuntimeStore` entry (press/hold timers,
  Tuya action de-dup state corrupted across unrelated devices) and never
  getting a def cache, so EVERY frame re-walked the 5500-def × 3-pass
  registry on the radio task. Capped to `ZAP_MAX_DEVICES` (single source
  of truth, included from `zap_common`); both `g_slots` (~4.8 KB) and the
  grown `g_store` (~10.4 KB) moved to PSRAM via `EXT_RAM_BSS_ATTR`
  (warm-path, never ISR) so the bump costs zero internal DRAM — the
  32-entry `g_store` previously sat in internal `.bss`, so this nets a
  small dram0 *saving* on S3. On genuine overflow the device is dropped
  with a log-once `ESP_LOGE`, never aliased to slot 0. (F §9 def 1, :221)
- **zhc_adapter**: `g_slots` / `g_slot_count` were mutated lock-free from
  five tasks (radio `try_decode`, `configure`, command `dispatch_and_send`,
  interview `register_endpoint`, httpd `resolve_supplement`) → duplicate
  slots + torn `cached_def` reads. Added `s_slots_mtx` (global-ctor mutex,
  matching the fallback pool's `PoolMtxInit` style) guarding the
  append-only table and all lookups; callers now go through
  `snapshot_slot` / `store_cached_defs` / `clear_cached_defs_for` so the
  radio task never dereferences `g_slots` without the lock. Hold times are
  short — NO registry walk / synth / radio I/O under the mutex.
  **Lock order**: `s_slots_mtx` and the fallback `g_pool_mtx` are NEVER
  nested — slot resolve releases before any `owns()`/synth takes the pool
  lock, and the eviction walk (`invalidate_cached_defs_in`) holds the pool
  lock but touches `g_slots` via word-atomic pointer clears + the
  append-only invariant, so no inversion is possible. (F §9 def 2, :219)
- **zhc_adapter**: `try_decode` (radio RX hot path) blocked `portMAX_DELAY`
  on a shared addressing mutex (`g_cfg_addr_mtx`) that `zhac_adapter_
  configure` held across `run_configure` — including 1500 ms Wait steps
  and bind/report radio round-trips — causing multi-second frame-intake
  stalls and a deadlock if a configure hook awaited a response the blocked
  RX task delivers. **Removed the cross-call address globals
  (`g_cfg_ieee` / `g_cfg_nwk` / `g_cfg_addr_mtx`) entirely.** The
  library's `configure_*` bridge fn-ptrs carry the per-call
  `device_index`; since the slot table is append-only that index stably
  identifies the device, so each bridge now resolves its OWN destination
  from `g_slots[idx]` (`ieee` + a new per-slot `cfg_nwk`, latched by
  `zhac_adapter_set_runtime_addr` / `configure`). `try_decode` takes NO
  lock for addressing — a configure on one device and a decode on another
  use different slots and can't collide. The Tuya MCU sync-time / dataQuery
  response path threads the per-call nwk through `ctx.device_nwk` from the
  same slot, so converter replies route to the correct node.
  (F §9 defs 3+4, :1086 / :1087)
- **zhc_adapter**: unmatched devices are now negative-cached — a resolved
  no-match tags the slot with a distinct `kMissSentinel` def-ptr (separate
  from the `cached_supplement == primary` sentinel) so their frames skip
  the 5500-def registry re-walk; cleared on `register_endpoint` /
  `fallback_clear` / `invalidate_def_cache` so a device that later gains
  cluster data and matches via synth is not masked. The per-frame "no
  definition" resolve log demoted INFO → DEBUG. (F §9 def 5, :1001)
- **zhc_adapter**: `dispatch_and_send` (command path) re-resolved the
  definition on every command (full registry walk, or a fallback rebuild
  for synth devices). It now reuses the per-IEEE `cached_def` that
  `try_decode` maintains — snapshotted under `s_slots_mtx` and revalidated
  via `zhc_fallback::owns()` (one-generation A/B lifetime rule) before
  use, falling back to a fresh resolve only on miss. (F §9 def 6, :1137)
- **zhc_adapter**: the ESP-IDF one-shot timer pool raced between the caller
  (`idf_timer_schedule` / `_cancel`, decode/configure tasks) and the
  `esp_timer` service task (`idf_timer_fire_thunk`) on the slot's `in_use`
  / `handle` fields — a `_cancel` concurrent with a fire opened a
  double-`esp_timer_delete` window and an ABA cancel of a reused slot id.
  Slot transitions now run under a `portMUX` critical section (spinlock —
  fire-thunk must not block), the deletable handle is claimed under the
  lock and deleted outside it, and the `TimerId` is generation-tagged
  (`gen << 16 | slot+1`) so a cancel of a slot reused since the id was
  minted is a no-op. (F §9 def 7, :166)
- **zhc_adapter (CMake)**: added `zap_common` to `PRIV_REQUIRES` for the
  `ZAP_MAX_DEVICES` header.
- **HW-GATE**: host build cannot exercise the radio path. Pending hardware
  verification: decode burst during another device's configure (no
  stall/deadlock), Tuya sync-time frames route to the correct nwk, and the
  33rd+ device gets a real decode + its own runtime state (no index-0
  aliasing).

### Fixed — High (P1 findings review, zigbee_pool)

- **zigbee_mgr / zigbee_pool**: new locked-visitor API
  `zigbee_pool_with_device(ieee, fn, ctx)` /
  `zigbee_pool_with_device_by_nwk(nwk, fn, ctx)` — runs `fn` under the
  pool's internal recursive mutex (the same one `zigbee_pool_lock()` and
  `pool_remove()` take), so the `ZapDevice*` handed to `fn` cannot be
  retargeted by a concurrent swap-with-last remove for `fn`'s duration.
  `pool_find_by_ieee` / `pool_find_by_nwk` doc-comments now spell out the
  hazard: the returned pointer is only valid under `zigbee_pool_lock()` —
  snapshot under lock (house pattern, `zhc_shadow_bridge.cpp`) or use the
  visitor; plus `zigbee_pool_snapshot(ieee, out)` /
  `zigbee_pool_snapshot_by_nwk(nwk, out)` — locked find+copy convenience
  wrapping the snapshot pattern for pure-copy callers. (F6/F35)
- **zigbee_mgr**: `zcl_attr_task` no longer dereferences the
  `pool_find_by_nwk` pointer across the liveness write and the
  multi-second `zhac_adapter_try_decode` — find + liveness stamp +
  522 B snapshot now happen under one lock and the decode runs on the
  detached copy (`zigbee_mgr.cpp:742` pre-fix). `on_zdo_leave_ind`
  soft-removes via the locked visitor and marks NVS-dirty from a
  detached snapshot outside the mutex (`:801` pre-fix). (F6/F35)
- **zigbee_mgr**: `do_interview` no longer writes through a stale pool
  pointer across multi-second blocking ZNP I/O (the documented F6/F35
  residual at `zigbee_interview.cpp:269`). The ZDO/Basic pipeline now
  runs on a detached `work` copy snapshotted in the existing
  find-or-create critical section; results are committed field-by-field
  under a re-acquired lock at the end — preserving concurrent mid-interview
  mutations (rejoin `nwk_addr` refresh, rename, leave/rejoin flags,
  liveness) and yielding to the late-identity promotion path
  (`zigbee_identity.cpp`) when it won the race, exactly as the old code
  did. If the device was hard-removed mid-interview the commit is
  skipped — results dropped, no resurrection. Rejoin fast-path
  (`:499` pre-fix) and the per-attempt nwk re-read (`:537` pre-fix) in
  `task_interview`, plus `zigbee_interview_trigger`, now hold the lock
  across find + read/mutate. (F6/F35)
- **zigbee_pool**: `s_hash_dirty` was missing `static` — internal hash
  state leaked into the global namespace.

### Fixed — High (P1 findings review, event_bus)

- **event_bus**: drain-vs-unsubscribe use-after-free — `event_bus_drain`
  read the subscriber table, blocked in `xQueueReceive`, and invoked the
  handler with no lock at all; a concurrent `event_bus_unsubscribe` could
  `vQueueDelete` the very queue the drain was blocked on and null the
  `std::function` it was about to call. Drains now resolve the slot under
  the bus lock, pin the queue with a per-slot `inflight` count across the
  unlocked receive, and re-validate the subscription generation after every
  wake before dispatching (on a private handler copy, taken once per drain
  call). Unsubscribe never deletes a queue with users in flight: it marks
  the slot dying, wakes blocked drainers with a poison event, and the queue
  is reaped by a later publish/subscribe/drain once `inflight` hits 0
  (`s_dying` gates the scan, so the publish hot path pays one compare).
- **event_bus**: publish held the single global mutex across the whole
  fan-out including user filter callbacks — one slow filter stalled every
  publisher, and filters re-entered the bus with the lock held (cross-task
  ordering deadlock surface; device_shadow publishes while holding its own
  mutex). Publish now snapshots queue handles + filter copies (copied only
  when non-null — no in-tree subscriber sets one) under the lock and
  evaluates filters / sends outside it, pinned by `inflight`.
- **event_bus**: handles are now generation-stamped
  (`[type|gen|pos]` packing, still `uint16_t`/opaque) and bumped on both
  subscribe and unsubscribe — a stale double-unsubscribe after slot reuse
  previously deleted the new subscriber's queue; now it is rejected and
  logged. Residual: `uint8_t` gen wraps after 256 sub/unsub cycles on one
  slot (documented; memory-safe either way, and nothing in-tree even calls
  unsubscribe today).
- **event_bus**: `xQueueCreate` failure in subscribe was `configASSERT`-only
  — with asserts compiled out the null queue was stored and the handler ran
  synchronously under the global lock. Now unlocks and returns
  `EVENT_SUB_INVALID` (the queue-less direct-handler publish path is gone
  with it).
- **event_bus**: new `event_bus_drain_handle(handle, timeout_ms)` drains
  exactly one subscription; `event_bus_drain(type, …)` is kept (deprecated
  by comment — `[[deprecated]]` would `-Werror` the in-tree single-
  dispatcher main loop, which intentionally drains all types in one task)
  and rebuilt on the same gen+inflight core, preserving its
  timeout-on-first-queue semantics.
- **event_bus**: unsubscribe also clears the slot's `filter` (its captures
  leaked before); `event_bus_init` is re-init-guarded (warn + no-op — a
  second call used to wipe the table, leaking every live queue and
  orphaning all subscriptions; house style per hap_session Q19);
  `EVENT_TYPE_COUNT` now derives from a new `EventType::_COUNT` enum
  sentinel instead of a hand-maintained `11` (source-only; the 96-byte
  payload ABI asserts are untouched). Host test harness added at
  `components/event_bus/test/host/` reusing the simple_rules shims (which
  gained a one-shot `stub_queue_fail_next_create()` knob); also removed a
  stray `</content>` paste artifact that ended this file.

### Fixed — High (P1 findings review, device_shadow)

- **device_shadow**: no flash I/O or bus publish under `s_mutex` any more —
  the shadow lock is now a leaf. Pre-fix, `device_shadow_process` (radio RX
  path) committed attr blobs to NVS while holding the lock (:412), the
  task_shadow sweep held it across the FULL 200-entry table doing sequential
  `nvs_set_blob`+`commit` (:446), and `emit_zcl_attr` published to the event
  bus under it (:334) — nesting the shadow lock over the bus lock (deadlock
  surface for any bus filter touching shadow API) and stalling the radio
  path, readers and the FreeRTOS timer task for tens-hundreds of ms per
  flash commit. ALL attr persistence is now a dirty-mark consumed by the
  task_shadow sweep, which cycles the locks per entry (lock → serialize into
  a task-owned PSRAM scratch + clear flags → unlock → publish + flash write
  outside). Failed writes re-mark dirty after the interval stamp, giving an
  `NVS_MIN_INTERVAL_S` backoff instead of hammering a failing flash every
  100 ms. Worst-case added persistence latency is one sweep period (~100 ms,
  was: immediate when interval-eligible). Config setters split the F26
  dedupe into decide-under-lock / write-outside-lock halves (`CfgPersist`),
  and `clear_attrs` erases its NVS key after unlock. Documented residual of
  that move: a sweep flash write already in flight when `clear_attrs` erases
  the key can land after the erase and resurrect the pre-clear blob until the
  next clear/sweep persist cycle — RAM state stays correct, and only a reboot
  inside that window would load the stale blob.
- **device_shadow**: ZCL_ATTR events are staged under the lock and published
  after `xSemaphoreGive` via a new `s_emit_mutex` (always taken BEFORE
  `s_mutex`, owns the shared PSRAM staging buffer, held across the publish)
  — preserves the old publish-order totality while the bus (hardened in
  `22217df`) runs filters in the publisher's task with no shadow lock held.
  Read-only API (`get_attrs`/`get_config`) takes `s_mutex` alone and is no
  longer stalled by publishes or flash writes.
- **device_shadow**: `device_shadow_restore_from_pool` mutated
  `s_shadow`/`s_count` and loaded through the shared `s_attr_blob` scratch
  with NO lock (:500), racing the already-spawned task_shadow iterating the
  table every 100 ms. NVS reads now land in a boot-only PSRAM scratch
  outside the lock, the table install runs under `s_mutex`, and
  `DEVICE_JOIN`/`zap_store_mark_dirty` fire after release. (Spawning
  task_shadow after restore instead was rejected — restore is called by
  firmware after `init()` returns, so reordering needs a split init/start
  API across both cores.)
- **device_shadow**: the occupancy-timeout callback blocked the SHARED
  FreeRTOS timer service task on `s_mutex` with `portMAX_DELAY` (:248) —
  while an NVS sweep held the lock, every software timer in the firmware
  froze. The callback is now lock-free: zero-timeout enqueue of
  `{ieee, kind}` onto the (renamed, unified) task_shadow queue, with a
  per-entry fallback flag + log-once when full; the synthetic occupancy=0
  runs on task_shadow under proper locking, and an `xTimerIsTimerActive`
  guard drops timeouts made obsolete by a fresh occupancy=1 re-arm (a race
  the old run-in-callback code also had, narrower).
- **device_shadow**: debounce `xTimerCreate` failure fell through to
  `xTimerReset(NULL)` and tripped `configASSERT` (:396); now guarded like
  the occupancy timer, with `debounce_pending_flush` set as fallback so the
  merged attrs flush via the sweep instead of sitting in `pending` forever.

**NEEDS HARDWARE TEST** — full 200-device sweep under live radio traffic,
occupancy timers firing during a sweep, and ZCL_ATTR emit-order parity
need on-device verification.

### Fixed — High (P1 findings review, flush writeback)

- **zap_store / rule_store**: flush protocol cleared a slot's dirty state
  BEFORE its NVS write completed (`zap_store_flush_slot` :81,
  `rule_store_flush_slot` :70 pre-fix), so a concurrent
  `flush_now`/`flush_device` saw "nothing dirty" and returned while the
  write was still in flight — breaking the shutdown/OTA durability
  contract (the P4/S3 flush-before-restart calls could reboot mid-write),
  and letting rule_store overlay readers fall through to stale NVS during
  the commit window. Both dirty tables now run a
  DIRTY → FLUSHING → CLEAN/FREE state machine (replacing the occupancy
  bool): the slot stays visible and owned for the whole NVS write
  (transitions under the existing mutex, I/O outside it), a `remarked`
  flag records marks that land mid-write (settle then leaves the slot
  dirty so the newer data flushes next cycle), and a failed write reverts
  the slot in place for next-tick retry — eliminating the old
  re-queue-into-a-free-slot path, which in rule_store could insert a
  stale snapshot alongside a newer mark for the same `rule_id`
  (duplicate entries; index-ordered flushing could persist the stale
  copy last, :88 pre-fix) and which needed the F37 full-table
  synchronous-save fallback (now superseded on the retry path; the
  `mark_dirty` table-full fallback is unchanged).
- **zap_store / rule_store**: `flush_now()` (and zap's
  `flush_device()`) are now true durability barriers: they wait
  (bounded poll, 10 ms × ≤5 s) for FLUSHING slots owned by the tick task
  to settle, with a second pass for slots re-marked during those writes
  — on return, state pending at call time is on flash. Both are
  task-context-only (vTaskDelay); all existing callers qualify
  (P4 `esp_register_shutdown_handler` handlers run in the task calling
  `esp_restart`; S3 OTA handler task).
- **zap_store**: `flush_device(ieee)` resolved the slot index under the
  mutex, released it, then flushed the index (:169 pre-fix) — the slot
  could settle and be reused for a different IEEE in that window,
  force-flushing the wrong device. The flush attempt now revalidates the
  expected IEEE under the flush lock, and slot reuse during a write is
  structurally impossible (the free-list skips non-FREE slots, and a
  FLUSHING slot is not FREE).

### Fixed — High (P1 findings review, NVS error propagation)

- **zap_common**: new header-only `nvs_checked.h` — `nvs_seq(&acc, op,
  TAG, "what")` logs every failing NVS op and accumulates the first
  error of a multi-op sequence, making the honest check-every-return
  pattern a one-liner. Host ctest suite added at `zap_common/test/host/`
  (local esp_err/esp_log shims with an ESP_LOGE counter).
- **zap_common**: polish fold — `nvs_seq` accepts `acc == nullptr` for
  best-effort sites that only want per-op logging (host test added);
  rule_store / device_shadow best-effort cleanups drop their dead
  accumulators. `device_shadow_clear_attrs` gates its summary line: the
  unconditional "Cleared" INFO becomes a WARN ("RAM cleared; NVS erase
  failed — cached attrs may resurface on reboot") when the erase/commit
  failed. Log tense honesty: "retried / re-wiped next boot" → "will
  retry / will re-wipe next boot" (zap_store, device_shadow, rule_store).
- **zap_store**: the schema-mismatch wipe ignored
  `nvs_erase_all`/`nvs_set_u16`/`nvs_commit` (:107 pre-fix) — a failed
  erase followed by the version overwrite would present stale-layout
  blobs as current. The version marker is now written only after a clean
  erase and every op is checked (first-boot marker write checked too).
  "Device saved" was logged even when the commit failed (:195 pre-fix);
  the log is gated on the commit now and the commit error is logged (the
  return value was already honest). The delete-path erase-all/rewrite
  loop ignored per-key erase/set errors and committed anyway
  (:259-:265 pre-fix) — it now aborts WITHOUT the commit on the first
  failure, so a half-compacted slot table can never become durable.
- **rule_store**: the boot-time schema check ignored its
  `set_u16`/`commit` returns (:91 pre-fix) — a failed marker write
  silently re-wiped the rule store on every boot. Each op now logs its
  failure (marker written only after a clean wipe). Corrupt-entry
  cleanup erases (load + load_all) stay best-effort but log failures.
- **rule_store**: tombstone tri-state (T9 follow-up).
  `rule_store_delete` collapsed "not found" and "erase/commit failed"
  into one `false`, and the flush settle forced tombstones ok — a
  genuine erase failure cleaned the tombstone and the deleted rule
  resurrected from NVS on reboot. New internal
  `rule_store_delete_err()` distinguishes `ESP_OK` /
  `ESP_ERR_NVS_NOT_FOUND` / failure; the flush settles not-found
  (nothing was stored) and keeps the slot pending for next-tick retry on
  real failures, per the T9 FLUSHING protocol. Public bool API and
  semantics unchanged (no external callers; not-found no longer spams
  ESP_LOGE from the tombstone path).
- **device_shadow**: the v7 version bump ignored
  `erase_all`/`set_u8`/`commit` (:469 pre-fix) — a failed erase plus the
  marker overwrite would hide stale v6 blobs behind the v7 marker. The
  marker is now written only after a clean erase; the attr-cache clear's
  `erase_key`+`commit` are checked too (not-found stays silent — the
  device may never have persisted).
- **zap_store / rule_store flush**: `xTaskCreate` returns were ignored
  and `s_task_started` stayed true on failure (zap :206 pre-fix) — dirty
  marks would defer into a table no task ever drains (silent persistence
  loss). On create failure the flag rolls back so marks fall through to
  the direct-save/delete path, and the failure is logged.
- **simple_rules**: the TIMER action's block-time-0
  `xTimerChangePeriod`/`xTimerReset`/`xTimerStart` (and `xTimerCreate`)
  returns were ignored (:364 pre-fix) — on a full timer command queue
  the timer action was silently lost. Arm failures now warn once per
  boot.
