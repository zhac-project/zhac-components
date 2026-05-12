# zap_store — NVS Device Persistence

NVS-backed pool persistence for `ZapDevice` records. Schema-versioned, CRC32-protected, with a deferred write-back cache that batches runtime updates and minimises flash wear.

## Where it sits

```
zigbee_mgr / device_backend
        │     mark_dirty / save / delete
        ▼
   ┌──────────┐
   │ zap_store │ ── NVS namespace "zap_v0"
   └──────────┘     keys: d0000…dXXXX (BLOB), cnt (U16), schema_ver (U16)
        │ flush task (1 s tick, prio 3)
        ▼
   esp_partition (NVS)
```

Two source files:

- `zap_store.cpp` — synchronous load/save/delete primitives + namespace bookkeeping.
- `zap_store_flush.cpp` — write-back cache + 1 s flush task.

## Public API (`include/zap_store.h`)

### Lifecycle

```cpp
void zap_store_init();        // init NVS_FLASH, open namespace, verify schema
bool zap_store_is_ready();    // true once init completed
void zap_store_flush_init();  // start 1 s flush task (idempotent)
```

### Synchronous primitives

```cpp
bool     zap_store_save_device  (const ZapDevice* dev);          // write/replace by IEEE
bool     zap_store_delete_device(uint64_t ieee);                 // remove + compact
uint16_t zap_store_load_devices (ZapDevice* pool, uint16_t cap); // returns count
```

### Deferred write-back (preferred for runtime updates)

```cpp
typedef enum {
    ZAP_PERSIST_LOW  = 0,   // ≤ 300 s latency — runtime bookkeeping
    ZAP_PERSIST_HIGH = 1,   // ≤   5 s latency — user-visible mutations
} ZapPersistPriority;

typedef bool (*ZapStoreSnapshotCb)(uint64_t ieee, ZapDevice* out);
void zap_store_set_snapshot_cb(ZapStoreSnapshotCb cb);

void zap_store_mark_dirty (const ZapDevice* dev, ZapPersistPriority pri);
void zap_store_flush_now  ();                  // shutdown / OTA — blocking
bool zap_store_flush_device(uint64_t ieee);    // force one entry — blocking
```

`mark_dirty` upgrades priority if the same IEEE is queued at LOW and is
re-marked HIGH. Without an installed snapshot callback, `mark_dirty` falls
back to an immediate `save_device` using the caller's pointer.

## Storage layout

NVS namespace **`zap_v0`**:

| Key | Type | Description |
|-----|------|-------------|
| `d0000` … `dXXXX` | BLOB (522 B) | One `ZapDevice` per slot, 0-based dense indexing |
| `cnt`             | U16         | Number of stored devices |
| `schema_ver`      | U16         | Current value: **6** (`ZAP_STORE_SCHEMA_VERSION`) |

On `zap_store_init`, a stored `schema_ver` mismatch triggers a full
`nvs_erase_all` of the namespace. v4→v5 added the CRC32 field; v5→v6
added the four lifecycle bytes (`interview_state`, `support_state`,
`configure_state`, `configure_attempts`). Struct grew 518→522 B.

## Compaction (delete)

```
Before: [d0000:A] [d0001:B] [d0002:C] [d0003:D]   cnt=4
delete B (idx 1):
   1. erase d0001
   2. shift C→d0001, D→d0002
   3. erase d0003
   4. cnt = 3
After:  [d0000:A] [d0001:C] [d0002:D]             cnt=3
```

Keeps storage dense so `load_devices` is a contiguous sweep with no holes.

## CRC32 integrity

Each `ZapDevice` carries a `crc32` field computed over the struct minus
the field itself. Computed on save, verified on load. Mismatched entries
are skipped with a warning and don't enter the in-memory pool.

## Write-back cache (zap_store_flush.cpp)

A small dirty-table keyed by IEEE; the flush task wakes every 1 s and
flushes entries whose age exceeds the priority threshold:

| Priority | Latency cap | Typical writers |
|----------|-------------|-----------------|
| `ZAP_PERSIST_HIGH` | 5 s    | rename, `interview_state` finalisation, removal |
| `ZAP_PERSIST_LOW`  | 300 s  | identity refresh, configure transitions, last-seen drift |

**Power-loss contract:** LOW data can lose up to 300 s on a hard crash;
HIGH up to 5 s. Graceful reboots and the OTA handoff register
`esp_register_shutdown_handler(zap_store_flush_now)` to flush first.

## Threading

- `zap_store.cpp` serialises every NVS access with an internal mutex.
- `zap_store_flush.cpp` owns its own mutex over the dirty-table.
- The flush task runs at priority 3 — flagged in `docs/FINDINGS.md` as
  ZB-F12 (multi-second commits can starve `rule_store` / `device_shadow`
  briefly during bulk recovery).

## CMakeLists

```cmake
idf_component_register(
    SRCS         "zap_store.cpp" "zap_store_flush.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     spi_flash nvs_flash zap_common freertos esp_timer
)
```

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| `nvs_flash_init` returns `ESP_ERR_NVS_NO_FREE_PAGES` / `NEW_VERSION_FOUND` | erase + retry once |
| schema_ver mismatch | wipe namespace, write current version |
| CRC32 mismatch on a slot | skip slot, log warning, continue load |
| `nvs_set_blob` / `nvs_commit` failure | log error, leave entry dirty for next tick |
| Snapshot callback returns false (device gone) | drop dirty entry silently |
| Dirty table full | log warning, oldest LOW-priority entry evicted |

## Sizing

- Per device: **522 B** (`sizeof(ZapDevice)`).
- Max devices: **200** (`ZAP_MAX_DEVICES`).
- NVS body footprint: ~104 KB at full capacity, plus NVS overhead.
- Recommended NVS partition: ≥ 128 KB.

## Integration example

```cpp
#include "zap_store.h"
#include "esp_heap_caps.h"

void boot() {
    zap_store_init();
    if (!zap_store_is_ready()) return;

    auto* pool = (ZapDevice*)heap_caps_calloc(
        ZAP_MAX_DEVICES, sizeof(ZapDevice),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t n = zap_store_load_devices(pool, ZAP_MAX_DEVICES);

    // Hook the snapshot callback so mark_dirty pulls the live state from
    // the in-memory pool instead of trusting a caller-supplied pointer.
    zap_store_set_snapshot_cb(my_pool_snapshot);
    zap_store_flush_init();
    esp_register_shutdown_handler(zap_store_flush_now);
}
```

## Cross-references

- `components/zap_common/README.md` — `ZapDevice` layout (522 B, schema v6)
- `components/zigbee_mgr/README.md` — primary writer (mark_dirty after interview / configure / rename / leave)
- `components/device_shadow/README.md` — separate NVS namespace; uses a similar mark-dirty pattern
- `docs/FINDINGS.md` — ZB-F11 (O(n²) bulk recovery), ZB-F12 (flush task priority)
