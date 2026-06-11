# Changelog

All notable changes to `zhac-components` are documented in this file. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow the platform-wide `vYYYYMMDDVV` scheme tagged from
`zhac-platform`.

## [Unreleased]

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
  and `clear_attrs` erases its NVS key after unlock.
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
