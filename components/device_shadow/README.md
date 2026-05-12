# device_shadow — ZCL Attribute Cache + Pipeline (P4)

Last-known-state cache for every Zigbee attribute, plus a per-device
filter / debounce / throttle / occupancy pipeline that sits between the
ZCL decoder and the rest of the firmware. Single source of truth for
"what does this device currently report"; persisted across reboots in
NVS, served from PSRAM at runtime.

## Where it sits

```
zigbee_mgr / zhc_adapter
        │ (decoded ZclAttribute*)
        ▼
device_shadow_process()           ← cache write + pipeline entry
        │
        ▼ shadow_pipeline_filter()
        ▼ debounce / throttle / occupancy
        │
        ├──► EventBus.publish(ZCL_ATTR)   ← downstream consumers
        ├──► hap_master broadcast          (S3 mirror)
        └──► NVS (debounced persist)       (zap_shadow namespace)
```

P4 only. The S3 holds a mirror populated from HAP `BULK_STATE_UPDATE`
frames and reads via REST/WS — it never imports this header.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` `zap_store` `event_bus` `nvs_flash` `freertos` `metrics`
`hap_protocol`. Header `device_shadow.h` pulls
`zcl_attribute.h` + `freertos/timers.h`.

## Public API (`include/device_shadow.h`)

### Lifecycle

| Symbol | Notes |
|---|---|
| `void device_shadow_init()` | Mounts NVS namespace `zap_shadow`, allocates the `DeviceShadowEntry[ZAP_MAX_DEVICES]` array in PSRAM, starts `task_shadow`. Idempotent. Call once from `zigbee_mgr_init` after `zap_store_init`. NVS open failure is non-fatal — logs `"cache will run without persistence"` and continues. |

### Main entry (from zigbee_mgr / zhc_adapter)

| Symbol | Notes |
|---|---|
| `void device_shadow_process(const ZapDevice* dev, const ZclAttribute* attrs, uint8_t count)` | Pipeline entry. Looks up / creates the `DeviceShadowEntry`, runs filter → debounce/throttle/occupancy → cache upsert → EventBus emit → NVS persist. Caller may pass any `count` ≤ 32; excess truncated. |
| `void device_shadow_update_optimistic(uint64_t ieee, const char* key, uint8_t val_type, int32_t int_val)` | Cache update path used after a successful command send when `DeviceConfig.optimistic == true`. Bypasses the pipeline so the UI sees the new value before the device reports it. |

### Cache read

| Symbol | Notes |
|---|---|
| `uint8_t device_shadow_get_attrs(uint64_t ieee, ShadowAttr* out, uint8_t max_count)` | Copies up to `max_count` cached attrs into `out`. Returns count copied; 0 means device unknown or empty cache. Thread-safe. |

### Per-device config

| Symbol | Notes |
|---|---|
| `bool device_shadow_set_config(uint64_t ieee, const DeviceConfig* cfg)` / `_get_config(...)` | Read/write the full middleware config blob. Persisted to NVS on set. False means device not found. |
| `bool device_shadow_set_debounce_ms(uint64_t ieee, uint32_t ms)` | Convenience setter for chatty devices (e.g. Tuya thermostats spamming ~10 msg/s). 0 disables. |
| `bool device_shadow_set_occupancy_timeout(uint64_t ieee, uint16_t s)` | TTL for `occupancy=1` → auto-clear to `0` after `s` seconds. 0 disables. Range 10–3600 enforced by REST. |

### Housekeeping

| Symbol | Notes |
|---|---|
| `void device_shadow_clear_attrs(uint64_t ieee)` | Wipes the cached attrs **and** the NVS blob — used when a device rejoins. |

### Pipeline internals (extern "C" for tests)

| Symbol | Notes |
|---|---|
| `shadow_pipeline_filter()` | Drop keys listed in `cfg->filtered`. |
| `shadow_pipeline_throttle_pass()` | Returns `true` once per `cfg->throttle_ms` window. |
| `shadow_pipeline_debounce_bypass()` | Returns the index in `cfg->debounce_ignore` if the key bypasses the debounce timer (e.g. `action` on a switch must always fire), else -1. |
| `shadow_pipeline_merge_pending()` | Last-write-wins merge into `PendingState.pending[32]`. |

## Important constants & sizes

| Symbol | Value | Source |
|---|---|---|
| `ATTR_KEY_MAX` | 28 (was 20 pre-v6) | `zcl_attribute.h` |
| `ATTR_STR_MAX` | 48 (was 32 pre-v6) | `zcl_attribute.h` |
| `sizeof(ShadowAttr)` | **84 B** (v6) | `static_assert` in header |
| `DEVICE_CONFIG_FILTER_MAX` | 8 | filter / debounce_ignore array depth |
| `DeviceShadowEntry.attrs[]` | 32 | hard cap per device |
| `PendingState.pending[]` | 32 | debounce coalesce buffer |
| `ZAP_MAX_DEVICES` | 200 | upper bound on entries |
| `NVS_SHADOW_VERSION` | **6** | bumped on every layout change; mismatch wipes the namespace |
| Worst-case PSRAM | 32 attrs × 200 devs × 84 B ≈ **537 KB** | grew from 384 KB at v5 |

## Wire format / on-disk layout

`ShadowAttr` (84 B, packed; static_asserted):

| Offset | Field | Notes |
|---|---|---|
| 0–27   | `char key[28]` | NUL-terminated attribute name |
| 28     | `uint8_t val_type` | `ValType` enum (INT / BOOL / STR) |
| 29     | `uint8_t flags`   | reserved (bit 0 reserved for SHA-F4 `FLOAT_X100`) |
| 30–31  | `uint16_t _pad`   | alignment |
| 32–35 / 32–79 | union `int_val` / `str_val[48]` | int or NUL-terminated string |
| 80–83  | `uint32_t ts`     | seconds since boot; 0 = never seen |

NVS keys: `zap_shadow / a_<ieee_hex>` for the attr blob,
`zap_shadow / c_<ieee_hex>` for the per-device `DeviceConfig`. The
namespace also stores the version byte so a layout change wipes all
entries cleanly.

## Threading & concurrency

- One internal task `task_shadow` (priority 4, 4 KB stack) owns the
  NVS write side and runs the debounce / occupancy timer callbacks.
- All public entry points take an internal recursive mutex.
- `device_shadow_process` uses two file-scope `static ZclAttribute
  filtered[32]` / `merge[32]` scratch buffers — safe because the mutex
  serializes callers and the alternative (5 KB on the stack) overflows
  the `zcl_attr` task on Xiaomi cube interview bursts.
- NVS persistence is throttled (`nvs_dirty` + `nvs_last_write_s`);
  the pipeline writes at most every few seconds per device to spare
  flash.
- `debounce_pending_flush` is the documented race fallback for
  SHA-F2: if the debounce timer fires while the rule queue is full,
  the flag re-arms a single deferred flush instead of dropping the
  buffer.

## Failure modes

| Condition | Behaviour |
|---|---|
| NVS open fails at init | Logs `"cache will run without persistence"`; cache stays in PSRAM only |
| `NVS_SHADOW_VERSION` mismatch on boot | Namespace wiped; v5 → v6 cleared automatically on first boot of new firmware |
| Per-attr blob size mismatch (older layout) | Single key erased, cache for that device empties on next report |
| `ZAP_MAX_DEVICES` reached | New IEEE rejected with `ESP_LOGW` |
| Debounce timer create fails | Update flushes immediately; no silent drop |
| Occupancy timer pool exhausted | Single `xTimerCreate` per device (lazy); failure logged once |

## Integration example

```c
// Boot:
zap_store_init();
device_shadow_init();
event_bus_init();

// Configure a chatty device:
DeviceConfig cfg{};
cfg.debounce_ms        = 1000;
cfg.occupancy_timeout_s= 120;
strncpy(cfg.filtered[0], "linkquality", ATTR_KEY_MAX-1);
cfg.filtered_count = 1;
device_shadow_set_config(0x00158D0001234567ULL, &cfg);

// Hot path (called from zhc_adapter shadow hook):
device_shadow_process(dev, decoded_attrs, n_attrs);

// Optimistic update on outbound command:
device_shadow_update_optimistic(ieee, "state", VAL_BOOL, 1);

// REST read:
ShadowAttr buf[32];
uint8_t n = device_shadow_get_attrs(ieee, buf, 32);
```

## Recent changes

- **2026-04-25 schema v6.** `ATTR_KEY_MAX` widened 20 → 28,
  `ATTR_STR_MAX` widened 32 → 48; `ShadowAttr` grew 60 → 84 B
  (SHA-F1 in `docs/FINDINGS.md`). NVS namespace bumped 5 → 6 — old
  cache wiped on first boot.
- **DeviceConfig string-keyed.** `filtered[]` / `debounce_ignore[]`
  now hold short attribute names (`"linkquality"`) instead of the
  retired `attr_key_id_t` enum.
- **SHA-F2 mitigation.** `debounce_pending_flush` flag re-arms the
  flush instead of dropping the buffer when the rule queue is full.

## Cross-references

- `docs/FINDINGS.md` § 3 (SHA-F1…SHA-F4 — known issues)
- `components/zap_common/include/zcl_attribute.h` — canonical
  `ZclAttribute` definition (84 B, key 28, str 48)
- `components/event_bus/README.md` — `ZCL_ATTR` event format
- `components/zhc_adapter/` — the upstream that calls
  `device_shadow_process`
- `zhac-main-core/main/zigbee_mgr.cpp` — wires the init
