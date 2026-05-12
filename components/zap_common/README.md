# zap_common — Shared Foundation Types

Header-only foundation component: protocol enums, the `ZapDevice` record, the schema-versioned `ZclAttribute` value, snapshot/journal/rule headers, and the cross-chip CPU sampler. Sits at the bottom of the dependency graph — every other component pulls from here, and `zap_common` itself depends on nothing.

## Where it sits

Pure types — no FreeRTOS, no NVS, no UART. Included by `zap_store`, `zigbee_mgr`, `device_shadow`, `event_bus`, `device_backend`, `zhc_adapter`, `simple_rules`, `lua_engine`, `hap_dispatch`, and the WS/REST handlers. Both P4 and S3 link against it.

## Files

| Header | Purpose |
|--------|---------|
| `include/zap_common.h` | `NcpProtocol`, `ZapDevice`, `RuleSlot`, `JournalEntry`, `SnapHeader`, lifecycle enums |
| `include/zcl_attribute.h` | `ZclAttribute` (schema v6, 84 B) + `ValType` + setter helpers |
| `include/sys_metrics.h` | `sys_metrics_sample_cpu_pct()` — per-call-site CPU sampler |

## Public API

### `NcpProtocol` (zap_common.h:8)

```cpp
typedef enum : uint8_t {
    PROTO_ZIGBEE = 0x00,   // default; zero-init structs land here
    PROTO_BLE    = 0x01,
    PROTO_THREAD = 0x02,
    PROTO_WIFI   = 0x03,
    PROTO_ZWAVE  = 0x04,
} NcpProtocol;

inline const char* ncp_protocol_name(NcpProtocol p);  // "zb" | "ble" | "th" | "wifi" | "zw" | "?"
```

### Lifecycle state enums (zap_common.h:48–70)

Backend-agnostic per-device state, persisted in `ZapDevice` so a device's
position in the **join → interview → match → configure** pipeline survives reboot.

```cpp
enum class InterviewState  : uint8_t { NONE, TOPOLOGY_READY, IDENTITY_PENDING, IDENTITY_READY, FAILED };
enum class SupportState    : uint8_t { UNKNOWN, UNMATCHED, MATCHED, PARTIAL };
enum class ConfigureState  : uint8_t { PENDING, IN_PROGRESS, DONE, FAILED };
```

### `ZapDevice` — 522 bytes packed (zap_common.h:83)

```cpp
struct ZapDevice {
    uint64_t ieee_addr;                // EUI64
    union { uint16_t nwk_addr;         // Zigbee short
            uint16_t conn_handle;      // BLE
            uint16_t rloc16;           // Thread
            uint16_t addr_raw; };
    uint8_t      endpoint_count;
    uint8_t      device_type;
    NcpProtocol  protocol;
    uint8_t      flags;                // bit0 = ZAP_DEV_REMOVED (soft-removed)
    uint32_t     last_seen;
    uint16_t     manufacturer_code;
    char         model_id[34];         // Basic 0x0005
    char         manufacturer_name[34];// Basic 0x0004
    char         friendly_name[30];
    uint8_t      endpoints[8];
    uint16_t     clusters[8][12];      // server-side (in)
    uint16_t     clusters_out[8][12];  // client-side (out, ZDO bind src)
    uint8_t      link_quality;
    uint8_t      power_source;
    uint16_t     battery_pct;
    uint8_t      interview_state;      // InterviewState
    uint8_t      support_state;        // SupportState
    uint8_t      configure_state;      // ConfigureState
    uint8_t      configure_attempts;
    uint32_t     crc32;                // verified on NVS load
} __attribute__((packed));
static_assert(sizeof(ZapDevice) == 522);     // zap_common.h:114
```

`flags` is repurposed from the old `_proto_pad`; helpers `zap_dev_is_removed`,
`zap_dev_mark_removed`, `zap_dev_clear_removed` live alongside the struct.

### `ZclAttribute` — 84 bytes packed (zcl_attribute.h)

Schema **v6** (widened 2026-04-25). Used everywhere a decoded ZCL value
crosses a task boundary or hits the shadow.

```cpp
static constexpr uint8_t ATTR_KEY_MAX = 28;   // was 20 in v5
static constexpr uint8_t ATTR_STR_MAX = 48;   // was 32 in v5

enum ValType : uint8_t { VAL_NONE=0, VAL_INT=1, VAL_BOOL=2, VAL_STR=3 };

struct __attribute__((packed)) ZclAttribute {
    char     key[ATTR_KEY_MAX];   // 0..27  null-terminated
    uint8_t  val_type;            // 28
    uint8_t  _pad;                // 29
    uint16_t cluster;             // 30..31  origin (0 if synthetic)
    uint16_t attr_id;             // 32..33  origin (0 if synthetic)
    uint16_t _pad2;               // 34..35
    union {
        int32_t int_val;                   // 36..39 (VAL_INT/BOOL)
        char    str_val[ATTR_STR_MAX];     // 36..83 (VAL_STR)
    };
};
static_assert(sizeof(ZclAttribute) == 84);
static_assert(offsetof(ZclAttribute, val_type) == ATTR_KEY_MAX);
static_assert(offsetof(ZclAttribute, int_val)  == 36);
```

Helpers (header-inline): `zcl_attr_set_int`, `zcl_attr_set_bool`, `zcl_attr_set_str`.
Define `ZCL_ATTR_ASSERT_KEY_FITS` to abort on key truncation in debug builds.

> **Why v6:** v5 silently truncated `color_temperature_startup` (25 chars) and
> compound action labels like `brightness_step_up_color_temperature_step_up`
> (43 chars). See SHA-F1 / SHA-F8 in `docs/FINDINGS.md`.

`NVS_SHADOW_VERSION` in `device_shadow.cpp` and `ZclAttrEvent._pad` in
`event_bus.h` (96-byte event contract) move in lockstep with these constants.

### `RuleSlot` — 536 bytes packed (zap_common.h:165)

NVS row for `rule_store`. Layout: `rule_id(2)+enabled(1)+_reserved(1)+
src_len(2)+trigger_type(1)+rule_type(1)+name(24)+src(500)+crc32(4)`.

```cpp
enum class RuleType    : uint8_t { SIMPLE=0, LEGACY_RESERVED_1=1 /* was BERRY */ };
enum class TriggerType : uint8_t { DEVICE_ATTR=1, TIME_CRON=2, EVENT=3,
                                   BOOT=4, TIMER=5, MQTT_TOPIC=6 };
static_assert(sizeof(RuleSlot) == 536);
```

512-byte v1 rows from older firmware fail length validation and are
discarded — early-dev stance, operators re-save on upgrade.

### `SnapHeader` — 64 bytes (zap_common.h:30)

```cpp
struct __attribute__((packed)) SnapHeader {
    uint32_t magic;        // 0x5A415053 'ZAPS'
    uint32_t sequence;     // monotonic, A vs B compared on boot
    uint16_t device_count;
    uint16_t attr_count;
    uint32_t crc32;
    uint8_t  _pad[48];
};
static_assert(sizeof(SnapHeader) == 64);
```

### `JournalEntry` — 12 bytes (zap_common.h:130)

```cpp
struct __attribute__((packed)) JournalEntry {
    uint8_t  type;          // ADD_DEVICE=1 UPDATE_ATTR=2 REMOVE=3 RENAME=4
    uint8_t  _pad;
    uint16_t payload_len;
    uint32_t seq;
    uint32_t crc32;
};
static_assert(sizeof(JournalEntry) == 12);
```

### `sys_metrics_sample_cpu_pct` (sys_metrics.h)

```cpp
static inline void sys_metrics_sample_cpu_pct(uint8_t& c0, uint8_t& c1);
```

Per-call-site rolling sampler over FreeRTOS idle counters. Each translation
unit that includes this header gets its own static baseline — never share
windows between two cadences (P4 heartbeat vs S3 `/api/status`). Returns 0
on the first call and whenever `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`
is off.

## Constants

| Symbol | Value | Source |
|--------|-------|--------|
| `ZAP_MAX_DEVICES`     | 200   | zap_common.h:178 |
| `ZAP_MAX_RULES`       | 256   | zap_common.h:179 |
| `ZAP_CLUSTERS_PER_EP` | 12    | zap_common.h:81  |
| `ATTR_KEY_MAX`        | 28    | zcl_attribute.h  |
| `ATTR_STR_MAX`        | 48    | zcl_attribute.h  |
| `ZAP_DEV_REMOVED`     | 0x01  | zap_common.h:117 |

## CMakeLists / dependencies

```cmake
idf_component_register(INCLUDE_DIRS "include")
```

No `REQUIRES` — pure header-only. Compiles equally on host (used by
`embedded/zhc/` host tests).

## Threading & concurrency

None of these types own a lock. Concurrency is enforced by the components
that hold them — `zigbee_pool`'s recursive mutex for `ZapDevice`,
`device_shadow`'s mutex for `ZclAttribute`, `rule_store`'s mutex for
`RuleSlot`. Callers are responsible for not tearing structs while readers
iterate (use `zigbee_pool_lock()` etc.).

## Integration

```cpp
#include "zap_common.h"
#include "zcl_attribute.h"

ZapDevice d{};
d.ieee_addr = 0x00124B0012345678ULL;
d.protocol  = PROTO_ZIGBEE;
d.interview_state = (uint8_t)InterviewState::IDENTITY_READY;

ZclAttribute a{};
zcl_attr_set_int (&a, "brightness", 128);
zcl_attr_set_str (&a, "action",     "single");
```

## Recent changes

- **2026-04-25** — `ZclAttribute` widened to 84 B (schema v6); `ATTR_KEY_MAX` 20→28, `ATTR_STR_MAX` 32→48 (SHA-F1/SHA-F8).
- **2026-04-22** — `RuleSlot` grew 512→536 B with `name[24]`.
- **2026-04-21** — `RuleType::BERRY` retired; slot kept as `LEGACY_RESERVED_1` for migration.
- **2026-04-19** — `ZapDevice._proto_pad` repurposed as `flags` (`ZAP_DEV_REMOVED`).

## Cross-references

- `docs/FINDINGS.md` — CC-F1 (ABI size drift), SHA-F1/F8/F11 (key truncation, padding asserts)
- `components/zap_store/README.md` — NVS persistence of `ZapDevice`
- `components/device_shadow/README.md` — NVS persistence of `ZclAttribute`
- `components/event_bus/README.md` — `ZclAttrEvent` 96-byte contract that gates `ATTR_STR_MAX`
- `embedded/zhc/` — host-built ZCL library that emits these `ZclAttribute` values
