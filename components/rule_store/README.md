# rule_store — Rule Persistence Layer (P4)

NVS-backed persistence for `RuleSlot` blobs with a PSRAM writeback
cache. `simple_rules` parses and dispatches; `rule_store` keeps the
authoritative on-flash copy. CRUD is synchronous, NVS commits are
batched on a 1 s tick task to spare flash wear in the REST edit path.

## Where it sits

```
REST / WS  ──► simple_rules_add/update/delete
                          │
                          ▼
                  rule_store_mark_dirty()        ─┐
                  rule_store_mark_delete()       ─┤  PSRAM dirty table (cap 64)
                                                  │
                                                  ▼
                                       1 s flush_task ──► nvs_set_blob / nvs_erase_key
                                                                   │
                                                          (nvs namespace "zap_rules")

simple_rules_init() ─► rule_store_load_all()  ←  reads dirty table first, then NVS
```

P4 only.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` `nvs_flash` `freertos` `esp_common`. `RuleSlot` is
defined in `zap_common.h` so this header is small (~50 lines).

## Public API (`include/rule_store.h`)

### Direct (synchronous) NVS CRUD

| Symbol | Notes |
|---|---|
| `void rule_store_init()` | Open NVS namespace `zap_rules` read/write. Idempotent; failure logs and disables persistence (cache stays in RAM). |
| `bool rule_store_save(const RuleSlot*)` | Compute CRC32, `nvs_set_blob` under key `r_%04X`, `nvs_commit`. Synchronous. |
| `bool rule_store_load(uint16_t rule_id, RuleSlot* out)` | Single-rule load. Validates blob length and CRC32; mismatch erases the key and returns false. |
| `bool rule_store_delete(uint16_t rule_id)` | `nvs_erase_key` + commit. |
| `uint16_t rule_store_load_all(RuleSlot* out, uint16_t max)` | Iterate the namespace; per-row size + CRC validation; corrupt rows are queued for erase and skipped. Returns count loaded into `out[0..max-1]`. |
| `bool rule_store_set_enabled(uint16_t, bool)` | Read-modify-write the enabled flag without touching the rest of the slot. |

### Writeback cache (preferred from REST path)

| Symbol | Notes |
|---|---|
| `void rule_store_flush_init()` | Spawn the 1 s flush task. Idempotent. |
| `void rule_store_mark_dirty(const RuleSlot*)` | Queue a save into the PSRAM dirty table. Flushed on age ≥ 5 s or on `rule_store_flush_now`. |
| `void rule_store_mark_delete(uint16_t rule_id)` | Queue a tombstone. Overrides any pending dirty-write for the same id. |
| `void rule_store_flush_now()` | Synchronous full flush. Call from shutdown handlers / OTA handoff. |

`rule_store_load` / `_load_all` consult the dirty table first, so
read-after-write is consistent inside the 5 s flush window.

## Important constants & sizes

| Symbol | Value | Source |
|---|---|---|
| NVS namespace | `zap_rules` | `rule_store.cpp` |
| NVS key format | `r_%04X` (e.g. `r_001A`) | per-rule blob key |
| `sizeof(RuleSlot)` | **536 B** | `static_assert` in `zap_common.h` |
| `ZAP_MAX_RULES` | 256 | upper bound on stored rules |
| `DIRTY_CAP` | 64 | concurrent dirty writeback slots |
| `FLUSH_TICK_MS` | 1000 | flush_task wake interval |
| `MAX_AGE_MS` | 5000 | age that triggers commit |
| Schema version | implicit via blob length | length mismatch = wipe row |

NVS partition sized to comfortably hold 256 × 536 B blobs + namespace
overhead (~150 KB).

## Wire format / on-disk layout

`RuleSlot` (536 B, packed; bumped 2026-04-22 from 512 B to add `name`):

| Offset | Field | Notes |
|---|---|---|
| 0–1   | `uint16_t rule_id` | 0 reserved; assigned by `simple_rules_add` |
| 2     | `uint8_t enabled` | 0/1 |
| 3     | `uint8_t _reserved` | was `version`; never set/checked, kept for ABI |
| 4–5   | `uint16_t src_len` | DSL text length |
| 6     | `uint8_t trigger_type` | `TriggerType` enum cache (parser hint) |
| 7     | `uint8_t rule_type` | `RuleType` enum (was `_pad`) |
| 8–31  | `char name[24]` | NUL-terminated UI label |
| 32–531 | `uint8_t src[500]` | DSL text (NUL-padded) |
| 532–535 | `uint32_t crc32` | over the full slot with `crc32` zeroed |

Every save recomputes CRC32 from a temp copy with the trailer zeroed;
every load verifies. Mismatch / wrong size → log + erase the key
(early-dev stance: prior 512-byte rows are discarded silently).

## Threading & concurrency

- One internal task: `flush_task` (low priority, small stack), wakes
  every `FLUSH_TICK_MS`.
- One mutex (`s_mtx`) protects the dirty table; the synchronous
  CRUD helpers lock briefly around `nvs_*` calls.
- `rule_store_flush_now` is the only path that drains the dirty
  table from the caller's context — used in shutdown / OTA handoff.

## Failure modes

| Condition | Behaviour |
|---|---|
| `nvs_open` fails at init | Logs error; CRUD returns false; `simple_rules` runs in RAM-only mode |
| Blob length ≠ `sizeof(RuleSlot)` on load | Logs `"size mismatch key=r_XXXX — queued for erase"`, erased on the same iteration |
| CRC32 mismatch on load | Logs `"CRC32 mismatch key=r_XXXX — queued for erase"`, erased |
| Power loss inside the 5 s flush window | Up to 5 s of REST edits lost — accepted trade for flash wear |
| `DIRTY_CAP` exceeded | Mark functions log warning; oldest writes flushed inline |
| `nvs_set_blob` ENOSPC | Logs; `simple_rules_add` propagates failure to UI |

## Integration example

```c
// Boot:
rule_store_init();
rule_store_flush_init();
RuleSlot slots[ZAP_MAX_RULES];
uint16_t n = rule_store_load_all(slots, ZAP_MAX_RULES);
ESP_LOGI(TAG, "loaded %u rules", n);

// REST edit path (called from simple_rules_update):
RuleSlot slot{};
slot.rule_id = id;
slot.enabled = 1;
slot.src_len = (uint16_t)strlen(dsl);
strncpy(slot.name, name, sizeof(slot.name)-1);
memcpy(slot.src, dsl, slot.src_len);
rule_store_mark_dirty(&slot);   // returns immediately; commit within 5 s

// Shutdown / OTA:
rule_store_flush_now();
```

## Recent changes

- **2026-04-22 RuleSlot v2.** `name[24]` and `rule_type` field added,
  blob grew 512 → 536 B. Old 512-byte rows fail length validation
  and are erased on first load (early-dev migration).
- **PSRAM writeback cache** (mirrors `zap_store_flush`): REST edits
  queue into the dirty table; the 1 s flush_task batches commits to
  spare flash. Reads consult the dirty table for consistency.

## Cross-references

- `components/simple_rules/README.md` — sole user of this store
- `components/zap_common/include/zap_common.h` — `RuleSlot` definition
  + `static_assert(sizeof(RuleSlot) == 536)`
- `docs/FINDINGS.md` — see ZB-F12 (NVS flush priority pitfalls)
