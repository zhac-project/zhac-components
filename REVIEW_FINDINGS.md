# Production-Readiness Code Review — zhac-components

**Date:** 2026-05-29  
**Reviewer:** Senior embedded systems architect  
**Scope:** All shared ESP-IDF components under `components/`  
**Methodology:** Line-by-line source analysis of every .cpp/.h file across 16 components

---

## Executive Summary

The codebase is well-engineered for an embedded system. Thread safety, CRC integrity, NVS atomicity, and buffer overflow protection are consistently addressed. The code shows evidence of prior security reviews (FINDINGS.md references throughout). That said, several issues remain — ranging from a critical credential leak to medium-severity concurrency gaps and low-severity robustness improvements.

---

## CRITICAL

### C-01: Telegram bot token logged in plaintext

- **Severity:** Critical
- **Category:** Credential handling / Security
- **Location:** `tg_gw/tg_gw_s3.cpp:91-92`
- **Confidence:** High
- **Description:** `tg_gw_handle_settoken` logs `"TG_SETTOKEN persisted len=%u %s"` which includes the token length and status. While the token itself isn't in this log line, the `tg_perform_send` function at line 183 constructs the URL `https://api.telegram.org/bot<TOKEN>/sendMessage` in a stack buffer. If ESP-IDF's HTTP client logs the URL (which it does at `ESP_LOG_DEBUG` level for `esp_http_client`), the full bot token is exposed in the serial log.
- **Why it matters:** Anyone with serial monitor access (or log streaming via WebSocket/MQTT) can extract the bot token and impersonate the controller's Telegram bot — sending messages to the operator's chat or receiving commands.
- **Suggested fix:** Set `cfg.log_level` on the HTTP client config to `ESP_LOG_NONE`, or ensure the URL is never logged. Consider zeroing the `url` buffer after `esp_http_client_cleanup`.

---

### C-02: Telegram bot token and chat ID stored in NVS without encryption

- **Severity:** Critical
- **Category:** Credential handling / Security
- **Location:** `tg_gw/tg_gw_s3.cpp:21-23,67-74`
- **Confidence:** High
- **Description:** The Telegram bot token (`tg_token`) and chat ID (`tg_chat`) are stored as plaintext NVS strings in the `"zhac"` namespace. NVS on ESP32 is stored in flash without encryption by default (unless flash encryption is enabled in menuconfig, which is not the default for development builds).
- **Why it matters:** Physical access to the flash chip (or a firmware dump) reveals the bot token. Combined with the chat ID, an attacker gains full control of the Telegram bot interface.
- **Suggested fix:** Use NVS encryption (enabled via `CONFIG_NVS_ENCRYPTION`) or store credentials in the encrypted flash partition. At minimum, document that flash encryption must be enabled for production deployments.

---

### C-03: WiFi credentials stored in plaintext NVS

- **Severity:** Critical  
- **Category:** Credential handling / Security
- **Location:** `zap_common/include/nvs_namespaces.h:63` (comment: "Plaintext today — encryption is a TODO")
- **Confidence:** High
- **Description:** The `wifi_cfg` namespace stores WiFi STA and AP credentials in plaintext. The code itself documents this as a known gap referencing "CC-F8 in docs/FINDINGS.md".
- **Why it matters:** WiFi PSK extraction from flash gives an attacker network access. This is acknowledged in-code but remains unresolved.
- **Suggested fix:** Enable NVS encryption for the `wifi_cfg` namespace. This is a known TODO — prioritize for production.

---

## HIGH

### H-01: `hap_session_init` leaks mutex on re-init

- **Severity:** High
- **Category:** Memory / Resource leak
- **Location:** `hap_session/hap_session.cpp:84-86`
- **Confidence:** High
- **Description:** `hap_session_init()` calls `xSemaphoreCreateMutex()` unconditionally without checking if `s_mutex` already exists. If called twice (e.g., during a reconnection flow), the old mutex is leaked.
- **Why it matters:** Each leaked mutex consumes ~80 bytes of kernel memory. On a system with limited heap, repeated re-inits could exhaust memory.
- **Suggested fix:** Guard with `if (!s_mutex) s_mutex = xSemaphoreCreateMutex();` (same pattern used in `zap_store_init` and `event_bus_init`).

---

### H-02: `event_bus_drain` accesses subscriber table without lock

- **Severity:** High
- **Category:** Thread safety
- **Location:** `event_bus/event_bus.cpp:137-154`
- **Confidence:** High
- **Description:** `event_bus_drain()` reads `s_subs[idx][i].queue` and `s_subs[idx][i].handler` without acquiring `s_bus_mtx`. A concurrent `event_bus_unsubscribe()` could delete the queue (`vQueueDelete`) between the null-check at line 145 and the `xQueueReceive` at line 147 — use-after-free.
- **Why it matters:** Hard fault or data corruption on the subscriber task. The drain path is the hot path for every event consumer.
- **Suggested fix:** Acquire `s_bus_mtx` (recursive) for the duration of the drain loop, or snapshot queue handles under lock before draining.

---

### H-03: `hap_session_on_receive` — seen_ring not protected by mutex

- **Severity:** High
- **Category:** Thread safety / Race condition
- **Location:** `hap_session/hap_session.cpp:56-66, 217-231`
- **Confidence:** High
- **Description:** `seen_recently()` and `mark_seen()` access `s_seen_ring[]` and `s_seen_head` without holding `s_mutex`. If `hap_session_on_receive` is called from multiple tasks (e.g., SPI RX task and a retransmit-ACK path), the ring buffer can be torn-read.
- **Why it matters:** A torn read could cause a legitimate duplicate to be accepted (processing the same command twice) or a fresh frame to be falsely rejected as a duplicate.
- **Suggested fix:** Protect `s_seen_ring`/`s_seen_head` with `s_mutex`, or use a lock-free ring with atomic head/tail indices.

---

### H-04: `hap_encode` — CRC16 computed over uninitialized buffer bytes when payload is null

- **Severity:** High
- **Category:** Correctness / Information leak
- **Location:** `hap_protocol/hap_protocol.cpp:41-43`
- **Confidence:** High
- **Description:** When `f.payload_len > 0` but `f.payload == nullptr`, the `memcpy` at line 42 is skipped, but `hap_crc16(buf + OFF_PAYLOAD, f.payload_len)` at line 43 still computes a CRC over `f.payload_len` bytes of whatever was in `buf` at those positions. The encoded frame then contains uninitialized memory bytes with a valid CRC — the receiver accepts garbage.
- **Why it matters:** The receiver gets a frame that passes CRC validation but contains whatever was previously in the buffer (potentially sensitive data from prior frames, or zero-initialized memory that the application interprets as valid payload).
- **Suggested fix:** Return 0 (encode failure) when `f.payload_len > 0 && f.payload == nullptr`, or zero-fill the payload area before CRC computation.

---

### H-05: `device_shadow` — occupancy timer callback takes mutex from timer task context

- **Severity:** High
- **Category:** Concurrency / Deadlock risk
- **Location:** `device_shadow/device_shadow.cpp:221-246`
- **Confidence:** Medium
- **Description:** `occupancy_timeout_cb` runs in the FreeRTOS timer service task context and calls `xSemaphoreTake(s_mutex, portMAX_DELAY)`. If the timer task is the same task that holds `s_mutex` (e.g., `task_shadow` is blocked on the mutex while the timer daemon tries to acquire it), this creates a priority inversion or deadlock scenario. The code releases and re-acquires the mutex twice (lines 224-236 and 240-242), which compounds the risk.
- **Why it matters:** Deadlock in the timer service task blocks ALL software timers system-wide, not just this one.
- **Suggested fix:** Post a message to `s_debounce_queue` (like `debounce_timer_cb` does) instead of taking the mutex directly from the timer callback. Handle occupancy timeout in `task_shadow`.

---

### H-06: `simple_rules` — `s_dispatch_depth` is not atomic and not mutex-protected

- **Severity:** High
- **Category:** Thread safety
- **Location:** `simple_rules/simple_rules.cpp:26, 411, 432, 444`
- **Confidence:** High
- **Description:** `s_dispatch_depth` is a plain `uint8_t` incremented/decremented in `dispatch_event()` without any synchronization. `dispatch_event` is called from the event bus subscriber context, and if multiple event types fire simultaneously (ZCL_ATTR + RULE_EVENT), two tasks could race on this variable.
- **Why it matters:** The depth limiter protects against infinite event loops (rule fires event → event triggers rule → ...). A race could let the depth exceed `MAX_DISPATCH_DEPTH`, causing stack overflow from recursive dispatch.
- **Suggested fix:** Use `std::atomic<uint8_t>` or protect with the existing `s_mutex`.

---

## MEDIUM

### M-01: `hap_session` — `now_ms()` uses tick count multiplication that can overflow

- **Severity:** Medium
- **Category:** Correctness / Integer overflow
- **Location:** `hap_session/hap_session.cpp:68-70`
- **Confidence:** High
- **Description:** `now_ms()` returns `(uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)`. After ~49.7 days of uptime at 1ms tick rate, the uint32_t wraps. The subtraction `ms - ws.sent_ms` at line 272 works correctly for unsigned wrap (modular arithmetic), but only if the timeout is less than the wrap period. This is fine for 1000ms timeouts, but should be documented.
- **Why it matters:** Not a bug per se (unsigned subtraction handles wrap), but the cast from `TickType_t * portTICK_PERIOD_MS` could lose precision if `TickType_t` is 16-bit on some configs.
- **Suggested fix:** Use `esp_timer_get_time() / 1000` (as `zap_store_flush.cpp` does) for consistency and to avoid tick-rate dependency.

---

### M-02: `zap_store_delete_device` — power loss between erase-all and rewrite loses all devices

- **Severity:** Medium
- **Category:** Persistence / Atomicity
- **Location:** `zap_store/zap_store.cpp:252-274`
- **Confidence:** High
- **Description:** The delete operation erases all old device keys (lines 257-260), then writes back the compacted set (lines 261-264), then commits (line 267). If power is lost between the erase loop and the commit, all device records are lost. The code acknowledges this is "atomic read-all / erase-all / rewrite / commit" but the NVS commit only guarantees atomicity for individual operations, not for the sequence.
- **Why it matters:** Power loss during device deletion could wipe the entire device database (up to 200 devices), requiring all devices to re-pair.
- **Suggested fix:** Write the compacted set to temporary keys first, commit, then erase old keys and update `cnt` in a second commit. Or accept the risk and document it (the current approach is reasonable for a device-delete operation which is rare).

---

### M-03: `device_shadow` — static buffers in `apply_pipeline_and_emit` prevent multi-instance use

- **Severity:** Medium
- **Category:** Design / Reentrancy
- **Location:** `device_shadow/device_shadow.cpp:319, 345, 347, 389`
- **Confidence:** High
- **Description:** `filtered[32]`, `bypass[32]`, `merge[32]`, and `out[32]` are `static` local arrays. The code documents that "Caller holds `s_mutex` for the entire duration" making them safe for the current single-mutex design. However, this makes the function non-reentrant and fragile — any future code path that calls it without the mutex will silently corrupt state.
- **Why it matters:** These statics are a maintenance hazard. Any refactoring that splits the shadow lock or adds a second caller will introduce hard-to-diagnose corruption.
- **Suggested fix:** Accept the risk (documented in-code) or move to per-entry heap buffers. The current approach is pragmatic for the memory-constrained target.

---

### M-04: `dsl_parser` — `s_last_error` is a global mutable string, not thread-safe

- **Severity:** Medium
- **Category:** Thread safety
- **Location:** `simple_rules/dsl_parser.cpp:18-24`
- **Confidence:** High
- **Description:** `s_last_error[96]` is written by `dsl_set_err()` and read by `dsl_last_error()`. The code notes "Not thread-safe but dsl_parse is invoked under simple_rules_add's mutex anyway." However, `dsl_last_error()` is called from HAP handlers (outside the mutex) to populate error responses.
- **Why it matters:** If two tasks call `dsl_parse` concurrently (one from `simple_rules_add`, one from `simple_rules_update`), the error string can be torn. The HAP handler reading `dsl_last_error()` could see a half-written string.
- **Suggested fix:** Return the error string via an output parameter of `dsl_parse()` instead of a global, or protect with a dedicated mutex.

---

### M-05: `hap_json` — test file uses wrong function signatures

- **Severity:** Medium
- **Category:** Test coverage / Build correctness
- **Location:** `hap_json/test/main/test_hap_json.cpp:12,16`
- **Confidence:** High
- **Description:** The test calls `hap_json_encode_heartbeat(buf, sizeof(buf), &len, 3600, 180000)` with 5 arguments, but the actual function signature is `hap_json_encode_heartbeat(uint8_t*, size_t, uint16_t*, const HapHeartbeat&)` taking a struct. Similarly, `hap_json_decode_heartbeat(buf, len, uptime)` takes 3 args but the real signature expects `(const uint8_t*, uint16_t, HapHeartbeat&)`.
- **Why it matters:** The tests won't compile against the current API. This means the test suite is either stale or not being run in CI.
- **Suggested fix:** Update the test to construct a `HapHeartbeat` struct and pass it correctly. Verify tests compile and run in CI.

---

### M-06: `cron_parser` — `parse_field` uses `strtok_r` which modifies the input buffer

- **Severity:** Medium
- **Category:** Correctness
- **Location:** `cron_parser/cron_parser.cpp:53-85`
- **Confidence:** High
- **Description:** `parse_field` copies the input to a local `buf[128]` then uses `strtok_r` on it. If the input string is longer than 127 characters, it's silently truncated. For cron expressions this is unlikely (typical max is ~30 chars), but a maliciously crafted expression could exploit the truncation to change semantics.
- **Why it matters:** A truncated cron expression could match unintended times. For example, a very long comma-separated list in one field could be truncated mid-value, changing the schedule.
- **Suggested fix:** Reject expressions where any single field exceeds a reasonable length (e.g., 64 chars) before copying to `buf`.

---

### M-07: `rule_store` — `rule_store_load_all` holds mutex during NVS iteration + blob reads

- **Severity:** Medium
- **Category:** Performance / Latency
- **Location:** `rule_store/rule_store.cpp:179-231`
- **Confidence:** High
- **Description:** The function holds `s_mutex` for the entire NVS iteration (potentially 256 blob reads of 536 bytes each = ~137 KB of flash reads). On ESP32, NVS reads involve flash cache operations that can block. This blocks all other rule_store operations (save, delete, load) for the duration.
- **Why it matters:** During `simple_rules_reload()` (which calls `load_all`), the REST API's rule CRUD operations are blocked for potentially 50-100ms. Not catastrophic but noticeable for UI responsiveness.
- **Suggested fix:** Snapshot the key list under lock, release lock, read blobs without lock, re-acquire lock for merge. Or accept the latency (it's a rare operation).

---

### M-08: `tg_gw_s3` — static `body` and `text_esc` buffers in `tg_perform_send`

- **Severity:** Medium
- **Category:** Thread safety
- **Location:** `tg_gw/tg_gw_s3.cpp:190-191`
- **Confidence:** High
- **Description:** `body[3500]` and `text_esc[3200]` are `static` locals in `tg_perform_send`. The function runs on the single `TgWorker` task, so there's no concurrent access today. However, if the queue depth increases or a second worker is added, these become a data race.
- **Why it matters:** Maintenance hazard — adding a second worker task would silently corrupt outgoing Telegram messages.
- **Suggested fix:** Move to task-local storage or allocate on the worker's stack (16 KB stack has room).

---

### M-09: `hap_protocol` — `hap_decode_stream` resync scan misses 4th preamble byte

- **Severity:** Medium
- **Category:** Protocol correctness
- **Location:** `hap_protocol/hap_protocol.cpp:89-93`
- **Confidence:** High
- **Description:** The resync scan checks for `HAP_PREAMBLE[0]`, `[1]`, and `[2]` (0xAA, 0x55, 0xFE) but does NOT check `[3]` (the version byte 0x04). A candidate preamble with a different version byte will trigger a RESYNC return, but the subsequent decode attempt will fail with `HAP_DECODE_BAD_VERSION` rather than continuing the scan.
- **Why it matters:** In a noisy SPI environment, a 3-byte match with wrong version is plausible. The caller would need to re-invoke `hap_decode_stream` on the remaining buffer, which it likely does — but it adds an extra round-trip and a misleading BAD_VERSION diagnostic.
- **Suggested fix:** Add `buf[i+3] == HAP_PREAMBLE[3]` to the resync scan condition.

---

### M-10: `zap_store` — `delete_device` uses `free()` on PSRAM-allocated buffer

- **Severity:** Medium
- **Category:** Memory management
- **Location:** `zap_store/zap_store.cpp:208-212, 280`
- **Confidence:** High
- **Description:** The buffer is allocated with `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` (line 208) with a fallback to `std::calloc` (line 212), but always freed with `free()` (line 280). On ESP-IDF, `free()` correctly routes to `heap_caps_free()` which handles both PSRAM and internal allocations, so this is technically correct. However, mixing `heap_caps_calloc` with `free` is inconsistent and could break if ESP-IDF changes its allocator routing.
- **Why it matters:** Portability and code clarity. If the ESP-IDF allocator behavior changes, this could corrupt the heap.
- **Suggested fix:** Use `heap_caps_free()` consistently, or track which allocator was used and free accordingly.

---

## LOW

### L-01: `hap_protocol` — `HAP_FRAME_OVERHEAD` comment says "13 hdr+HDR_CRC16" but HDR_CRC is CRC8

- **Severity:** Low
- **Category:** Documentation
- **Location:** `hap_protocol/include/hap_protocol.h:9`
- **Confidence:** High
- **Description:** Comment says "13 hdr+HDR_CRC16 + 2 payload CRC16" but the header CRC is CRC-8 (1 byte at offset 12), not CRC-16. The overhead math is correct: 13 header bytes (including 1-byte CRC8) + 2 payload CRC16 bytes = 15.
- **Suggested fix:** Fix comment to "13 hdr+HDR_CRC8 + 2 payload CRC16".

---

### L-02: `hap_session` — test uses hardcoded window size of 8, but implementation uses 16

- **Severity:** Low
- **Category:** Test coverage
- **Location:** `hap_session/test/main/test_hap_session.cpp:69-84`
- **Confidence:** High
- **Description:** Test "session: 9th NEEDS_ACK while 8 outstanding returns false" assumes `WIN_SIZE=8`, but the implementation defines `WIN_SIZE=16`. The test sends 8 frames and expects the 9th to fail, but with `WIN_SIZE=16` it will succeed.
- **Why it matters:** The test is silently passing when it shouldn't (or failing when it should). It doesn't actually test the window-full condition.
- **Suggested fix:** Update test to send 16 frames before expecting the 17th to fail, or expose `WIN_SIZE` as a testable constant.

---

### L-03: `event_bus` — `EVENT_TYPE_COUNT = 11` but `EventType` enum goes to 10

- **Severity:** Low
- **Category:** Correctness / Maintenance
- **Location:** `event_bus/event_bus.cpp:13` vs `event_bus/include/event_bus.h:8-20`
- **Confidence:** High
- **Description:** `EVENT_TYPE_COUNT` is hardcoded to 11, while the `EventType` enum's highest value is `RULE_TIMER_FIRE = 10`. This works (indices 1-10, with 0 unused), but if a new event type is added with value 11, the count must be bumped to 12. There's no `static_assert` to catch drift.
- **Suggested fix:** Derive `EVENT_TYPE_COUNT` from the enum: `static constexpr uint8_t EVENT_TYPE_COUNT = static_cast<uint8_t>(EventType::RULE_TIMER_FIRE) + 1;`

---

### L-04: `metrics` — `counter_set` has a race with concurrent `counter_inc`

- **Severity:** Low
- **Category:** Concurrency
- **Location:** `metrics/src/metrics.cpp:187-197`
- **Confidence:** High
- **Description:** `counter_set` writes the target value to the current shard and zeros others. If another core calls `counter_inc` between the zeroing of shard 0 and the write to shard 1, the increment is lost. The code documents this as intentional ("Replace the aggregate"), but the window exists.
- **Why it matters:** For metrics, this is acceptable — `counter_set` is used for absolute gauges (heap size), not monotonic counters. The race window is nanoseconds.
- **Suggested fix:** Document the constraint: "Do not call counter_set concurrently with counter_inc on the same MetricId."

---

### L-05: `simple_rules` — `expand_value` only replaces first occurrence of `%value%`

- **Severity:** Low
- **Category:** Feature / Correctness
- **Location:** `simple_rules/simple_rules.cpp:125-137`
- **Confidence:** High
- **Description:** `expand_value` uses `strstr` to find the first `%value%` and replaces it. If the action template contains multiple `%value%` placeholders, only the first is substituted.
- **Why it matters:** A DSL rule like `DO publish topic "%value% changed to %value%"` would produce `"42 changed to %value%"`.
- **Suggested fix:** Loop until no more `%value%` occurrences are found, or document the single-substitution behavior.

---

### L-06: `device_shadow` — `nvs_save_attrs` key truncates IEEE to 56 bits

- **Severity:** Low
- **Category:** Correctness
- **Location:** `device_shadow/device_shadow.cpp:74`
- **Confidence:** High
- **Description:** The NVS key format `"a%014llX"` masks with `0x00FFFFFFFFFFFFFF` (56 bits), dropping the top 8 bits of the 64-bit IEEE address. Zigbee IEEE addresses are OUI-based and the top byte is typically 0x00 or 0x80-0xFF for locally administered addresses. Two devices differing only in the top byte would collide.
- **Why it matters:** Extremely unlikely in practice (Zigbee OUIs don't use the top byte this way), but theoretically possible.
- **Suggested fix:** Use full 16-hex-digit format: `"a%016llX"`.

---

### L-07: `cron_parser` — `cron_next` can infinite-loop on `mktime` returning -1

- **Severity:** Low
- **Category:** Robustness
- **Location:** `cron_parser/cron_parser.cpp:215-216, 240-241, 252-253`
- **Confidence:** Medium
- **Description:** When `mktime` returns `(time_t)-1`, `cron_next` returns 0. However, if the system clock is set to a far-future date near `time_t` max, the `limit` calculation at line 200 (`from_t + 4*365*24*3600`) could itself overflow, making the loop condition `t < limit` always true.
- **Why it matters:** On ESP32 with a misconfigured RTC, this could spin indefinitely in the cron task.
- **Suggested fix:** Add a maximum iteration count (e.g., 4 years × 366 × 24 × 60 × 60 ≈ 126M, but cap at something reasonable like 1M iterations).

---

### L-08: `hap_json` — `hap_json_decode_sync` uses `strncpy` without guaranteed NUL termination on exact-fit

- **Severity:** Low
- **Category:** Robustness
- **Location:** `hap_json/hap_json.cpp:101-102`
- **Confidence:** High
- **Description:** `strncpy(out.fw_ver, fv, sizeof(out.fw_ver) - 1)` followed by explicit NUL termination is correct. However, the pattern is inconsistent — some decoders use this safe pattern while others (e.g., `hap_json_decode_alert` at line 227) also do it correctly. The codebase is consistent here — no actual bug.
- **Suggested fix:** No change needed — the pattern is correctly applied throughout.

---

### L-09: `device_backend` — no mutex protecting backend registry

- **Severity:** Low
- **Category:** Thread safety
- **Location:** `device_backend/device_backend.cpp:9-28`
- **Confidence:** Medium
- **Description:** `s_backends[]` and `s_backend_count` are accessed without synchronization. Registration happens at startup (single-threaded), and lookups happen at runtime (multi-threaded). Since registration is write-once and lookups are read-only, this is safe in practice.
- **Why it matters:** If a backend were ever registered after startup (hot-plug), this would race.
- **Suggested fix:** Document the "register-only-during-init" contract, or add a mutex for defensive programming.

---

### L-10: `hap_protocol` — no validation of `HapMsgType` range in decode

- **Severity:** Low
- **Category:** Protocol robustness
- **Location:** `hap_protocol/hap_protocol.cpp:69`
- **Confidence:** High
- **Description:** `out.type = static_cast<HapMsgType>(buf[OFF_TYPE])` accepts any byte value 0x00-0xFF, including undefined message types. The receiver must handle unknown types gracefully.
- **Why it matters:** A corrupted or malicious frame with an unknown type code passes decode validation and reaches the application dispatcher, which must then handle it.
- **Suggested fix:** Add an optional `hap_msg_type_valid()` check, or document that receivers must handle unknown types. The current approach is defensible for a closed SPI bus.

---

## INFO

### I-01: Excellent use of CRC32 integrity checks on persistent records

Both `zap_store` and `rule_store` use CRC32 on every persisted record, with validation on load and automatic erasure of corrupt entries. The `device_shadow` attr blob uses a versioned header with CRC. This is production-grade persistence design.

### I-02: Well-designed writeback cache with priority semantics

The `zap_store_flush` and `rule_store_flush` components implement a clean dirty-table with HIGH/LOW priority, PSRAM-backed storage, and graceful fallback to synchronous write when the table is full. The shutdown/OTA flush hooks ensure durability.

### I-03: Good separation of concerns in HAP protocol layer

The protocol is cleanly split into framing (`hap_protocol`), session management (`hap_session`), and serialization (`hap_json`). The stage-1/stage-2 DMA transport is well-documented.

### I-04: Metrics engine is zero-allocation and lock-free

The sharded atomic approach with `memory_order_relaxed` is appropriate for observability metrics. The X-macro registry pattern ensures compile-time consistency between metric IDs and descriptors.

### I-05: DSL parser has good input validation

The parser validates IEEE literals (rejecting `0xGARBAGE`), bounds-checks all string copies, limits action count, and provides specific error messages via `dsl_last_error()`.

---

## Test Coverage Gaps

| Component | Has Tests | Gaps |
|-----------|-----------|------|
| hap_protocol | Yes | No test for `hap_encode` with null payload + nonzero len (H-04) |
| hap_session | Yes | Window size mismatch in tests (L-02); no concurrent send/receive test |
| hap_json | Yes | Tests use wrong API signatures (M-05); no bulk/escape tests |
| event_bus | No | No test file found — critical gap for the pub/sub backbone |
| zap_store | Yes | No test for concurrent save+delete; no power-loss simulation |
| device_shadow | Yes (pipeline) | No integration test for debounce/occupancy timers |
| simple_rules | Yes (DSL, matcher) | No test for dispatch depth limiter; no cron cache invalidation test |
| rule_store | Yes | No test for writeback overlay merge |
| cron_parser | Yes | No test for 6-field (seconds) form; no edge case for leap seconds |
| metrics | Yes | No test for sharded aggregation |
| mqtt_gw | No | No test file found |
| tg_gw | No | No test file found |
| zhc_adapter | No | No test file found |
| device_backend | No | No test file found |

---

## Summary by Severity

| Severity | Count |
|----------|-------|
| Critical | 3 |
| High | 6 |
| Medium | 10 |
| Low | 10 |
| Info | 5 |

**Top priorities for production:**
1. C-01/C-02: Telegram credential protection (enable flash encryption)
2. C-03: WiFi credential encryption (already tracked as TODO)
3. H-02: Event bus drain use-after-free
4. H-03: Seen ring buffer race
5. H-05: Occupancy timer deadlock risk
