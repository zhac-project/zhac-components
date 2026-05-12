# device_backend — Protocol-Agnostic Device Vtable Registry

Tiny abstract layer that lets `zigbee_backend`, `ezsp_backend`, and (planned) `c6_backend` / `ble_backend` plug into the same call surface. Higher layers — REST/WS handlers, rules engine, HAP dispatch — only see `DeviceBackend*`; they never depend on which radio is wired up.

## Where it sits

```
api_handlers.cpp / ws_bridge / hap_dispatch / simple_rules / lua_engine
                           │
                           │  device_backend_find(PROTO_*) / find_by_ieee(...)
                           ▼
                ┌─────────────────────┐
                │   device_backend    │  THIS COMPONENT
                │  registry (max 4)   │
                └──────────┬──────────┘
                           │ vtable lookup
       ┌───────────────────┼─────────────────────┐
       ▼                   ▼                     ▼
zigbee_backend       ezsp_backend          (future) c6_backend
       │                   │                     │
       ▼                   ▼                     ▼
 znp_driver           ezsp_driver           c6_driver  (UART)
```

> **Caveat (ZB-F3):** `zigbee_mgr_init` still calls ZNP-direct
> `MT_SREQ` paths in places, so `is_running()` accuracy depends on the
> backend reaching past the abstraction. Tracked in `docs/FINDINGS.md`.

## CMakeLists

```cmake
idf_component_register(
    SRCS         "device_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     zap_common log
)
```

Pure registry; no FreeRTOS, no NVS. Allocates statically — leaks
flagged in older docs were stale: the table is a `static
DeviceBackend* s_backends[DEVICE_BACKEND_MAX]` populated by
back-ends at startup.

## Public API (`include/device_backend.h`)

### `DeviceBackend` vtable

```cpp
static constexpr uint8_t DEVICE_BACKEND_MAX = 4;

struct DeviceBackend {
    NcpProtocol protocol;       // PROTO_ZIGBEE / PROTO_BLE / PROTO_THREAD / ...
    const char* name;           // "Zigbee" / "EZSP" / "BLE"

    // Lifecycle
    bool   (*init)(void);
    bool   (*poll)(void);                            // nullptr if backend owns its own task
    bool   (*is_running)(void);

    // Discovery
    bool   (*start_discovery)(uint8_t duration_s);   // permit_join / BLE scan
    bool   (*stop_discovery )(void);
    bool   (*interview)(uint64_t ieee, uint16_t addr_hint);

    // Attribute I/O — semantic / key-based
    bool   (*write_attr)(uint64_t ieee, uint8_t ep, const char* key, int32_t val);
    bool   (*read_attr )(uint64_t ieee, uint8_t ep, const char* key);

    // Device management
    bool   (*get_device_list)(ZapDevice* out, uint16_t max, uint16_t* count_out);
    bool   (*get_device    )(uint64_t ieee, ZapDevice* out);
    bool   (*remove_device )(uint64_t ieee);
    bool   (*rename_device )(uint64_t ieee, const char* name);
};
```

`ep == 0` means "use the device default endpoint" — the backend
resolves to `dev->endpoints[0]` (QWEN §11). Multi-endpoint devices
(dual switches, multi-sensor plugs) used to lose attribute writes
because every call routed to endpoints[0]; callers should now pass the
explicit endpoint from the request.

### Registry

```cpp
bool            device_backend_register   (DeviceBackend* b);   // false if full or duplicate proto
DeviceBackend*  device_backend_find       (NcpProtocol proto);  // by protocol tag
DeviceBackend*  device_backend_find_by_ieee(uint64_t ieee);     // iterate, ask each get_device()
uint8_t         device_backend_count();
DeviceBackend*  device_backend_get        (uint8_t index);
```

`find_by_ieee` is O(N · backend_lookup_cost). With 4 back-ends and an
O(1) pool lookup that's effectively constant; if BLE adds a
peer-table scan the cost can rise.

## Constraints

| Symbol                | Value | Source |
|-----------------------|-------|--------|
| `DEVICE_BACKEND_MAX`  | 4     | device_backend.h:9 |

## Threading

No internal locks today — registration happens at startup, lookup is
read-only thereafter. Each backend's vtable methods may have their own
synchronization (e.g. `zigbee_pool_lock` inside `get_device_list`).

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| `register` called > `DEVICE_BACKEND_MAX` | log E `backend registry full`, return false |
| Two backends register the same `protocol` | log W `already registered`, second one rejected |
| `find` for unregistered protocol | returns `nullptr` |
| `find_by_ieee` for unknown IEEE | returns `nullptr` |
| `get_device == nullptr` in some entry | skipped during `find_by_ieee` |

## Implementing a backend

```cpp
#include "device_backend.h"

static bool my_init()                                                           { /*...*/ return true; }
static bool my_is_running()                                                     { /*...*/ }
static bool my_start_discovery(uint8_t duration_s)                              { /*...*/ }
static bool my_stop_discovery()                                                 { /*...*/ }
static bool my_interview(uint64_t ieee, uint16_t addr_hint)                     { /*...*/ }
static bool my_write_attr(uint64_t ieee, uint8_t ep, const char* k, int32_t v)  { /*...*/ }
static bool my_get_device_list(ZapDevice* out, uint16_t max, uint16_t* count)   { /*...*/ }
static bool my_get_device(uint64_t ieee, ZapDevice* out)                        { /*...*/ }
static bool my_remove_device(uint64_t ieee)                                     { /*...*/ }
static bool my_rename_device(uint64_t ieee, const char* name)                   { /*...*/ }

static DeviceBackend g_my = {
    .protocol         = PROTO_BLE,
    .name             = "BLE",
    .init             = my_init,
    .poll             = nullptr,           // own task, no external pump
    .is_running       = my_is_running,
    .start_discovery  = my_start_discovery,
    .stop_discovery   = my_stop_discovery,
    .interview        = my_interview,
    .write_attr       = my_write_attr,
    .read_attr        = nullptr,           // not implemented
    .get_device_list  = my_get_device_list,
    .get_device       = my_get_device,
    .remove_device    = my_remove_device,
    .rename_device    = my_rename_device,
};

bool my_backend_register() { return device_backend_register(&g_my); }
```

## Cross-references

- `components/zigbee_backend/README.md` — primary ZNP adapter
- `components/ezsp_backend/README.md` — EFR32 adapter
- `components/c6_driver/README.md` — c6_backend planned (Phase 5 of the C6 plan)
- `components/zap_common/README.md` — `NcpProtocol` enum + `ZapDevice` struct
- `docs/FINDINGS.md` — **ZB-F3** (`zigbee_mgr_init` leaks past the abstraction), QWEN §11 (per-call endpoint contract)
