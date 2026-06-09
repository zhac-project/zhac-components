# Production-Readiness Code Review: zhac-components

**Date:** 2026-05-29
**Scope:** All 16 shared ESP-IDF components under `components/`
**Reviewer:** Senior embedded systems architect

---

## Executive Summary

The codebase demonstrates strong engineering discipline for an embedded dual-chip Zigbee controller. CRC integrity, schema versioning, mutex-guarded persistence, and PSRAM-aware allocation are consistently applied. The review identified **2 Critical**, **5 High**, **8 Medium**, **5 Low**, and **4 Info** findings. The two critical issues are a timer callback race in `zhc_adapter` and a use-after-free window in `event_bus_drain`.

---

## Critical Findings

### C-01: zhc_adapter timer fire thunk — TOCTOU race with cancel

- **Severity:** Critical
- **Category:** Thread safety / Use-after-free
- **Location:** `zhc_adapter/src/zhc_adapter.cpp:115-124` (`idf_timer_fire_thunk`)
- **Confidence:** High

**Description:** The esp_timer callback reads `slot->in_use`, fires the user callback, calls `esp_timer_delete`, and clears `in_use` — all without synchronization. `idf_timer_cancel` (line 164-174) can run concurrently from any task context: it checks `in_use`, calls `esp_timer_stop` + `esp_timer_delete`, and clears `in_use`. If cancel executes between the thunk's `in_use` check and its `esp_timer_delete` call, both paths call `esp_timer_delete` on the same handle — a double-free that corrupts the esp_timer internal allocator.

**Why it matters:** Double-free of an esp_timer handle causes heap corruption or an abort in `esp_timer_delete`'s internal assertions. This is reachable from any ZHC converter that schedules + cancels timers (e.g., Tuya MCU sync, occupancy auto-off).

**Suggested fix:** Guard the timer slot pool with a mutex, or use `esp_timer_stop` before delete in cancel and check the return value in the thunk. Alternatively, mark the slot `in_use = false` atomically before either path proceeds:
```cpp
void idf_timer_fire_thunk(void* arg) {
    auto* slot = static_cast<IdfTimerSlot*>(arg);
    if (!slot) return;
    // Atomically claim the slot
    if (!__atomic_exchange_n(&slot->in_use, false, __ATOMIC_ACQ_REL)) return;
    if (slot->fn) slot->fn(slot->device_index, slot->user_tag, slot->user_data);
    if (slot->handle) { esp_timer_delete(slot->handle); slot->handle = nullptr; }
}
```

---

### C-02: event_bus_drain — queue handle read without lock, use-after-free window

- **Severity:** Critical
- **Category:** Thread safety / Use-after-free
- **Location:** `event_bus/event_bus.cpp:137-154` (`event_bus_drain`)
- **Confidence:** High

**Description:** `event_bus_drain` reads `s_subs[idx][i].queue` (line 144) without holding `s_bus_mtx`, then enters a `while(xQueueReceive(q, ...))` loop. If another task calls `event_bus_unsubscribe` between the queue read and the `xQueueReceive`, the queue is deleted via `vQueueDelete` (line 92) while `drain` still holds the dangling handle. `xQueueReceive` on a deleted queue is undefined behavior — typically an access violation or data corruption.

**Why it matters:** Any subscriber that unsubscribes while its drain loop is active (e.g., module teardown during OTA) triggers this race. The event bus is the central nervous system — corruption here cascades to every subsystem.

**Suggested fix:** Hold `s_bus_mtx` across the drain loop, or snapshot the queue handle under lock and validate it hasn't been deleted before each receive:
```cpp
uint8_t event_bus_drain(EventType type, uint32_t timeout_ms) {
    uint8_t idx = static_cast<uint8_t>(type);
    if (idx == 0 || idx >= EVENT_TYPE_COUNT) return 0;
    bus_lock();
    // ... iterate under lock, but release for handler dispatch
}
```
Alternatively, document that `drain` and `unsubscribe` for the same type must not run concurrently, and enforce it with a per-subscriber "active" flag.

---

## High Findings

### H-01: DSL parser error string — non-thread-safe static buffer

- **Severity:** High
- **Category:** Thread safety
- **Location:** `simple_rules/dsl_parser.cpp:18-24` (`s_last_error`, `dsl_last_error`)
- **Confidence:** High

**Description:** `s_last_error` is a 96-byte static buffer written by `dsl_set_err` during `dsl_parse` and read by `dsl_last_error`. While `dsl_parse` is called under `s_mutex` from `simple_rules_add`/`simple_rules_update`, `dsl_last_error` is called from HAP dispatch handlers that may run on a different task. A concurrent parse + read produces a torn string.

**Why it matters:** The HAP RULE_CREATE handler reads `dsl_last_error()` to populate the error response. A torn read produces garbled UI error messages. Low probability but zero-cost to fix.

**Suggested fix:** Guard `s_last_error` with a mutex, or return a copy under the `simple_rules` recursive mutex.

---

### H-02: Test compilation failures — stale API references

- **Severity:** High
- **Category:** Test coverage / Build integrity
- **Location:** `hap_json/test/main/test_hap_json.cpp:12,16`, `rule_store/test/main/test_store.cpp:21`
- **Confidence:** High

**Description:**
1. `test_hap_json.cpp:12` calls `hap_json_encode_heartbeat(buf, sizeof(buf), &len, 3600, 180000)` — a 5-argument signature. The actual function takes `(uint8_t*, size_t, uint16_t*, const HapHeartbeat&)` — 4 arguments with a struct. The test cannot compile.
2. `test_hap_json.cpp:16` calls `hap_json_decode_heartbeat(buf, len, uptime)` with `uint32_t uptime` — but the actual signature takes `HapHeartbeat& out`.
3. `test_store.cpp:21` sets `s.version = 1` but `RuleSlot` has `_reserved` (renamed from `version`). This test cannot compile against the current struct.

**Why it matters:** Tests that don't compile provide zero coverage. These are the only tests for hap_json and rule_store — two components that handle all wire serialization and rule persistence.

**Suggested fix:** Update tests to match current API signatures. For heartbeat, construct a `HapHeartbeat` struct. For rule_store, use `_reserved` instead of `version`.

---

### H-03: cron_parser test semantic mismatch — 6-field rejection test

- **Severity:** High
- **Category:** Test correctness
- **Location:** `cron_parser/test/main/test_cron_parser.cpp:122-125`
- **Confidence:** High

**Description:** Test `"cron: too many fields returns false"` uses input `"* * * * * *"` (6 fields) and asserts `cron_parse` returns false. But `cron_parse` (line 109) explicitly accepts `n == 5 || n == 6` — the 6-field form is the documented seconds extension. This test asserts the opposite of the implementation's intended behavior.

**Why it matters:** If this test passes (it shouldn't against the current code), it means the 6-field feature is broken. If it fails, it blocks CI. Either way, the test/implementation contract is violated.

**Suggested fix:** Change the test input to 7 fields: `"* * * * * * *"` (which IS rejected at line 112). The existing 7-field test at line 252-255 already covers this correctly — remove the duplicate or fix it to test a genuinely invalid input.

---

### H-04: event_bus — no test file exists

- **Severity:** High
- **Category:** Test coverage gap
- **Location:** `event_bus/` — no `test/` directory
- **Confidence:** High

**Description:** The event bus is the central pub/sub backbone connecting every subsystem (shadow, rules, MQTT, HAP). It has zero test coverage. The subscribe/unsubscribe/publish/drain paths, the eviction policy, the recursive mutex behavior, and the filter predicate are all untested.

**Why it matters:** C-02 above demonstrates a real race condition in this component. Without tests, regressions in the locking model (already iterated through F28/F36 fixes) go undetected until they manifest as field crashes.

**Suggested fix:** Add host-testable tests covering: subscribe/unsubscribe lifecycle, publish with queue-full eviction, concurrent publish + unsubscribe, filter predicates, and drain semantics.

---

### H-05: simple_rules `s_dispatch_depth` — unprotected static across event bus contexts

- **Severity:** High
- **Category:** Thread safety
- **Location:** `simple_rules/simple_rules.cpp:26,411-444` (`s_dispatch_depth`)
- **Confidence:** Medium

**Description:** `s_dispatch_depth` is a plain `static uint8_t` incremented/decremented in `dispatch_event` (lines 432, 444). `dispatch_event` is registered as an event bus handler for 5 event types. If the event bus delivers events via direct handler invocation (line 131 of `event_bus.cpp`: `sub.handler(e)`) and a rule action publishes an event that re-enters `dispatch_event` on a different task, the counter is torn. The depth limit (MAX_DISPATCH_DEPTH=8) is the only guard against infinite event loops.

**Why it matters:** If the counter is corrupted, the depth limit either never triggers (infinite loop → stack overflow → watchdog reset) or triggers prematurely (legitimate events dropped).

**Suggested fix:** Make `s_dispatch_depth` `_Atomic uint8_t` or guard with `s_mutex`.

---

## Medium Findings

### M-01: hap_decode_stream — resync preamble check omits version byte

- **Severity:** Medium
- **Category:** Protocol robustness
- **Location:** `hap_protocol/hap_protocol.cpp:89-99`
- **Confidence:** High

**Description:** The resync scan checks `buf[i]==0xAA && buf[i+1]==0x55 && buf[i+2]==0xFE` but does not verify `buf[i+3]==HAP_VERSION`. A 3-byte magic coincidence in payload data (probability ~1/16M per byte position) causes a false resync candidate. The subsequent `hap_decode` call will reject it (version check at line 53), but `consumed` is already set to `i`, causing the caller to skip valid data.

**Why it matters:** Under heavy SPI noise, a false resync can skip a valid frame that follows the noise burst. The streaming decoder's value proposition is recovering from corruption — false positives undermine this.

**Suggested fix:** Add `buf[i+3] == HAP_VERSION` to the resync scan condition (line 92).

---

### M-02: device_shadow NVS key — IEEE masking drops top 8 bits

- **Severity:** Medium
- **Category:** Data integrity
- **Location:** `device_shadow/device_shadow.cpp:74,96,121,159`
- **Confidence:** Medium

**Description:** NVS keys are generated as `"a%014llX"` with `ieee & 0x00FFFFFFFFFFFFFFULL`, masking the top byte. Two devices whose IEEE addresses differ only in the MSB (e.g., `0xAA11223344556677` and `0xBB11223344556677`) collide on the same NVS key. The second device's shadow cache overwrites the first's.

**Why it matters:** IEEE addresses are manufacturer-assigned. While collisions on the top byte are unlikely in a single deployment (most devices share an OUI prefix), a fleet with mixed vendors could hit this. The shadow cache silently corrupts — the UI shows wrong last-known-state.

**Suggested fix:** Use the full 64-bit IEEE in the key: `"a%016llX"` (16 hex chars + prefix = 17 chars, within NVS's 15-char key limit — actually exceeds it). Alternative: use a hash or the device pool index as the key.

---

### M-03: tg_gw_s3 — Telegram bot token stored in plaintext NVS

- **Severity:** Medium
- **Category:** Security / Credential handling
- **Location:** `tg_gw/tg_gw_s3.cpp:21-22,89` (NVS_NS="zhac", NVS_TOKEN="tg_token")
- **Confidence:** High

**Description:** The Telegram bot token is stored as a plaintext NVS string. NVS on ESP-IDF is unencrypted by default (the `nvs_flash` API does not enable flash encryption). An attacker with physical access and a UART/flash reader can extract the token and impersonate the bot.

**Why it matters:** A stolen bot token allows sending messages to the configured chat, potentially including phishing links or social engineering attacks against the home automation operator.

**Suggested fix:** Enable NVS encryption (`nvs_flash_secure_init_partition`) for the namespace holding credentials, or at minimum document this as a known limitation with a TODO referencing CC-F8 from FINDINGS.md.

---

### M-04: mqtt_gw_s3 — broker URL and credentials in plaintext NVS

- **Severity:** Medium
- **Category:** Security / Credential handling
- **Location:** `mqtt_gw/mqtt_gw_s3.cpp:25,293` (`s_broker_url`), `nvs_namespaces.h:58` (`mqtt_cfg`)
- **Confidence:** High

**Description:** MQTT broker URL (potentially including username:password in the URI) is stored in plaintext NVS and held in a static char array. Same concern as M-03 — physical flash access exposes credentials.

**Why it matters:** MQTT credentials grant access to the home automation message bus — an attacker can inject commands to any device.

**Suggested fix:** Same as M-03 — NVS encryption or document as known limitation.

---

### M-05: zap_store_delete_device — swap-with-last changes device ordering

- **Severity:** Medium
- **Category:** Data consistency
- **Location:** `zap_store/zap_store.cpp:240-248`
- **Confidence:** High

**Description:** Delete uses swap-with-last to keep the array compact. This silently changes the NVS slot index of the swapped device. If any external system (e.g., the in-memory pool, a connected UI) holds a slot index reference, it now points to the wrong device. The IEEE index is invalidated (`s_idx_built = false`), but callers that cached a slot index between delete and the next save see stale data.

**Why it matters:** The device pool in `zigbee_mgr` uses array indices for iteration. A concurrent delete + read could act on the wrong device. Mitigated by the pool lock, but the zap_store layer doesn't document this contract.

**Suggested fix:** Document that `zap_store_delete_device` invalidates all slot indices, or switch to a tombstone approach that preserves ordering.

---

### M-06: rule_store_load_all — 1 KB stack allocation for bad_keys

- **Severity:** Medium
- **Category:** Stack pressure
- **Location:** `rule_store/rule_store.cpp:186-188`
- **Confidence:** Medium

**Description:** `char bad_keys[64][16]` allocates 1024 bytes on the stack. Combined with the NVS iterator state, mutex overhead, and the calling task's existing stack frame, this could pressure the 6144-byte `rule_flush` task stack.

**Why it matters:** Stack overflow on embedded systems causes silent corruption or watchdog resets. The `rule_flush` task already had its stack bumped from 3072 to 6144 (per the comment at line 242 of `rule_store_flush.cpp`).

**Suggested fix:** Reduce `kBadMax` to 16 (256 bytes) or allocate from heap.

---

### M-07: simple_rules task_cron — 1 KB stack arrays

- **Severity:** Medium
- **Category:** Stack pressure
- **Location:** `simple_rules/simple_rules.cpp:490-493`
- **Confidence:** Medium

**Description:** `fire_idx[16]` + `fire_id[16]` = 64 bytes, but combined with the `ParsedRule snap` (line 512, ~300 bytes), the `time_t`/`struct tm` from `cron_matches`, and the NVS/ESP_LOG call chain, the 8192-byte `rule_cron` stack is moderately pressured.

**Why it matters:** The cron task was already bumped from 4 KB to 8 KB (per `task_stacks.h:39`). Further growth (e.g., adding logging) could trip the canary.

**Suggested fix:** Move `fire_idx`/`fire_id` to static (protected by `s_mutex` which is already held) or heap-allocate the `ParsedRule snap`.

---

### M-08: zhc_adapter — g_merged registry array silently truncates at 8192

- **Severity:** Medium
- **Category:** Silent data loss
- **Location:** `zhc_adapter/src/zhc_adapter.cpp:229-237`
- **Confidence:** Medium

**Description:** `kMaxRegistry = 8192` caps the merged registry. The `add` lambda silently stops adding when the cap is reached. The comment says "Total ported defs ≈ 5500+ and growing." As vendors are added, the array will silently truncate — last-added vendors (tier_e registries) are dropped without any log message.

**Why it matters:** Devices from silently-truncated vendors appear as UNMATCHED, losing all automatic decode/configure functionality. No error is logged.

**Suggested fix:** Add an `ESP_LOGE` when `g_merged_count >= kMaxRegistry` during merge, and consider making `kMaxRegistry` a runtime-checked assertion.

---

## Low Findings

### L-01: hap_session — `now_ms()` uses tick count, not esp_timer

- **Severity:** Low
- **Category:** Timing accuracy
- **Location:** `hap_session/hap_session.cpp:68-70`
- **Confidence:** High

**Description:** `now_ms()` uses `xTaskGetTickCount() * portTICK_PERIOD_MS`, which has 1-10 ms resolution depending on `configTICK_RATE_HZ`. Other components (zap_flush, rule_flush, metrics) use `esp_timer_get_time() / 1000` for 1 ms resolution. The ACK timeout (1000 ms) has ±10 ms jitter from tick alignment.

**Why it matters:** Minimal — the 1000 ms timeout has generous margin. But inconsistent time sources across components make debugging timing issues harder.

**Suggested fix:** Use `esp_timer_get_time() / 1000ULL` for consistency.

---

### L-02: device_shadow — `persist_attrs_force` has inverted early-return

- **Severity:** Low
- **Category:** Logic error
- **Location:** `device_shadow/device_shadow.cpp:176-177`
- **Confidence:** Medium

**Description:** `persist_attrs_force` returns early when `!e->nvs_dirty && e->attr_count == 0`. The intent is "nothing to persist." But if `attr_count > 0` and `nvs_dirty == false`, it still writes — even if the attrs were already persisted (e.g., by a recent throttled write). This produces redundant NVS writes.

**Why it matters:** Extra flash wear. The 5-minute NVS_MIN_INTERVAL_S throttle in `persist_attrs_throttled` is bypassed by `persist_attrs_force`, which is called from `device_shadow_set_config`. Each config change triggers an unconditional attr write.

**Suggested fix:** Track whether attrs have changed since last persist, or check `nvs_last_write_s` in `persist_attrs_force`.

---

### L-03: tg_gw_s3 — chat_id logged in plaintext

- **Severity:** Low
- **Category:** Information disclosure
- **Location:** `tg_gw/tg_gw_s3.cpp:104-105`
- **Confidence:** High

**Description:** `ESP_LOGI(TAG, "TG_SETCHAT persisted chat=%s ...")` logs the chat ID. While less sensitive than the bot token, the chat ID identifies the recipient and could be used for targeted social engineering.

**Suggested fix:** Mask the chat ID in logs (e.g., show only last 4 digits).

---

### L-04: mqtt_gw_p4 — static mutex initialized on first call without atomic guard

- **Severity:** Low
- **Category:** Thread safety (initialization)
- **Location:** `mqtt_gw/mqtt_gw_p4.cpp:36-40`
- **Confidence:** Medium

**Description:** `static SemaphoreHandle_t s_mutex = nullptr; if (!s_mutex) s_mutex = xSemaphoreCreateMutex();` — if two tasks call `mqtt_gw_publish` simultaneously before `s_mutex` is initialized, both create a mutex and one leaks. On ESP32-P4, `mqtt_gw_publish` is called from the rule executor and HAP dispatch, which can run on different cores.

**Why it matters:** One-time mutex leak (~96 bytes). Unlikely to trigger in practice since init ordering ensures the first publish happens after boot, but not impossible.

**Suggested fix:** Initialize `s_mutex` in `mqtt_gw_init()` (which runs once at boot).

---

### L-05: hap_protocol — `hap_encode` computes CRC16 over uninitialized payload bytes when `payload==nullptr` and `payload_len==0`

- **Severity:** Low
- **Category:** Correctness
- **Location:** `hap_protocol/hap_protocol.cpp:43`
- **Confidence:** Low

**Description:** When `payload_len == 0`, `hap_crc16(buf + OFF_PAYLOAD, 0)` returns the init value 0xFFFF. The CRC bytes written are `0xFF, 0xFF`. On decode, `hap_crc16(buf + OFF_PAYLOAD, 0)` also returns 0xFFFF, matching the stored bytes. This is correct but fragile — the zero-length CRC is a constant, so any corruption of the CRC bytes themselves is caught, but the "payload" region is uninitialized buffer bytes.

**Why it matters:** No functional impact — the decode validates correctly. But the uninitialized buffer content between OFF_PAYLOAD and the CRC could leak information if the frame buffer is later inspected.

**Suggested fix:** Zero the payload region in `hap_encode` when `payload_len == 0`, or document that the CRC covers zero bytes.

---

## Informational Findings

### I-01: Consistent use of ArduinoJson across hap_json

All JSON serialization in `hap_json.cpp` (1479 lines) uses ArduinoJson's `JsonDocument` with stack allocation. Every encoder checks `doc.overflowed()` and `n >= cap`. Every decoder validates `deserializeJson` return code. String copies consistently use `strncpy` with explicit NUL termination. This is well-executed.

### I-02: Strong persistence integrity patterns

Both `zap_store` and `rule_store` implement CRC32 integrity checks on every record, schema version migration with automatic wipe, and deferred writeback with priority-based flush. The `device_shadow` adds a blob header with version + count + CRC. This is production-grade persistence engineering.

### I-03: Metrics engine — zero-allocation, lock-free design

The metrics engine uses static storage, per-core sharding, and `std::atomic` with relaxed ordering. The compare-exchange loops for atomic min/max are correct. The Prometheus and MQTT exporters handle truncation safely. The concurrent hammer test validates the sharding model.

### I-04: Comprehensive static_assert coverage

Packed structs (`ZapDevice`, `RuleSlot`, `ShadowAttr`, `ZclAttribute`, `ZclAttrEvent`, `ZclRawEvent`, `MqttMsgEvent`, `RuleEventPayload`, `RuleTimerPayload`) all have `static_assert(sizeof(...))` guards. `ZclAttribute` additionally asserts field offsets. This prevents silent layout drift — critical for NVS blob compatibility.

---

## Test Coverage Summary

| Component | Test File | Status |
|-----------|-----------|--------|
| hap_protocol | `test/test_hap_frame.cpp` | ✅ 12 tests, comprehensive |
| hap_session | `test/main/test_hap_session.cpp` | ✅ 6 tests, good |
| hap_json | `test/main/test_hap_json.cpp` | ❌ **Won't compile** (stale API) |
| zap_store | `test/main/test_zap_store_v0.cpp` | ✅ 4 tests, good |
| device_shadow | `test/main/test_pipeline.cpp` | ✅ 12 tests, pipeline only |
| device_backend | — | ⚠️ No tests |
| event_bus | — | ❌ **No tests** (critical gap) |
| metrics | `test/main/test_metrics.cpp` | ✅ 14 tests, excellent |
| simple_rules (DSL) | `test/main/test_dsl_parser.cpp` | ✅ 19 tests, good |
| simple_rules (matcher) | `test/main/test_matcher_executor.cpp` | ✅ 18 tests, good |
| rule_store | `test/main/test_store.cpp` | ❌ **Won't compile** (`s.version`) |
| cron_parser | `test/main/test_cron_parser.cpp` | ⚠️ 1 test semantically wrong |
| mqtt_gw | — | ⚠️ No tests (S3 logic untested) |
| tg_gw | — | ⚠️ No tests |
| zhc_adapter | — | ⚠️ No tests (tested via embedded-zhc host tests) |

**Priority test gaps to address:** event_bus (H-04), hap_json compilation fix (H-02), rule_store compilation fix (H-02).

---

## Recommendations (Priority Order)

1. **Fix C-01** (zhc_adapter timer race) — add atomic or mutex guard to timer slot pool
2. **Fix C-02** (event_bus drain UAF) — hold bus_lock across drain, or add subscriber active flag
3. **Fix H-02** (test compilation) — update hap_json and rule_store tests to current API
4. **Fix H-03** (cron test semantic) — fix 6-field test to use 7-field input
5. **Add H-04** (event_bus tests) — critical infrastructure needs coverage
6. **Fix H-01/H-05** (simple_rules thread safety) — guard `s_last_error` and `s_dispatch_depth`
7. **Address M-02** (shadow NVS key collision) — use full IEEE or hash-based keys
8. **Address M-08** (registry truncation) — add log on truncation, bump cap proactively
9. **Plan M-03/M-04** (credential encryption) — NVS encryption for production deployments
