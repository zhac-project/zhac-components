# ONBOARDING — zhac-components

You are an AI agent arriving on **zhac-components**, the collection of
shared ESP-IDF components both ZHAC firmware chips depend on. Read
top-to-bottom before touching anything.

---

## 1. Platform context

**ZHAC** = dual-chip ESP32 Zigbee Home Automation Controller.

- **ESP32-P4** = Zigbee coordinator (`zhac-main-core`).
- **ESP32-S3** = WiFi/REST/WS/MQTT gateway (`zhac-net-core`).
- They talk over SPI using the custom **HAP** binary protocol.
- A Preact SPA (`www-spa`) is bundled into S3 SPIFFS.

Data flow:

```
Device ── Zigbee ──► P4 EZSP/ZNP ── zhc_adapter ── device_shadow
                                                      │
                                                 HAP over SPI
                                                      ▼
                                                S3 hap_master
                                                      │
                                                  ws_server / mqtt_gw
                                                      ▼
                                                 Web UI / Broker
```

Everything between `zhc_adapter` and `hap_master` lives in **this
repo**.

### Repo split

Tag `v2026042301` (2026-04-23) is the baseline for all 7 repos:
`zhac-platform`, `embedded-zhc`, **`zhac-components`** *(this)*,
`zhac-main-core`, `zhac-net-core`, `www-spa`, `zhac-docs`.

---

## 2. What this repo owns

Ten shared components plus one vendored dependency. Each is a
stand-alone ESP-IDF component with its own `CMakeLists.txt`,
`include/`, optional `Kconfig`, and (per-file) LICENSE header.

### Component inventory

| Component | Role | License | Depends on |
|-----------|------|---------|------------|
| `zap_common` | Canonical `ZclAttribute` (52 B), `ValType`, shared constants | Apache-2.0 | — |
| `zap_store` | ZCL attribute key → canonical interned string | Apache-2.0 | `zap_common` |
| `event_bus` | In-process typed pub/sub | Apache-2.0 | `freertos` |
| `device_shadow` | NVS-backed string-keyed attribute cache (schema v4), debounce/throttle pipeline | Apache-2.0 | `zap_common` · `zap_store` · `event_bus` · `nvs_flash` · `attr_keys` |
| `device_backend` | Backend-agnostic device CRUD abstraction | Apache-2.0 | `zap_common` · `zap_store` |
| `zhc_adapter` | One-way bridge from `embedded-zhc` → firmware (decode/send/configure/exposes-JSON) | Apache-2.0 | `embedded-zhc` · `zap_common` · `device_shadow` |
| `hap_protocol` | HAP binary framing (wire format) | Apache-2.0 | — |
| `hap_session` | HAP transport session state machine | Apache-2.0 | `hap_protocol` |
| `hap_json` | HAP ↔ JSON codec (S3↔P4 wire payloads) | Apache-2.0 | `hap_protocol` · `arduinojson` |
| `metrics` | Prometheus-style counters / gauges | Apache-2.0 | — |
| `arduinojson` | Vendored JSON parser | **MIT** | — |

If a component shows up in firmware but not here, check
`zhac-main-core/components/` (P4-only: `lua_engine`, `ezsp_driver`,
`znp_driver`, `ezsp_backend`, `zigbee_backend`, `zigbee_mgr`,
`hap_slave`, `lua_cjson`) or `zhac-net-core/components/` (S3-only:
`cron_parser`, `rule_store`, `simple_rules`, `mqtt`, `mqtt_gw`,
`ws_server`, `hap_master`). Those are single-chip and stay with their
firmware.

### Layout

```
zhac-components/
├── components/
│   ├── zap_common/        (+ include/zcl_attribute.h — the canonical 52 B struct)
│   ├── zap_store/
│   ├── event_bus/
│   ├── device_shadow/
│   ├── device_backend/
│   ├── zhc_adapter/       (bridge to embedded-zhc)
│   ├── hap_protocol/
│   ├── hap_session/
│   ├── hap_json/
│   ├── metrics/
│   └── arduinojson/       (MIT — vendored)
├── _TEMPLATE_LICENSE-Apache-2.0
├── _TEMPLATE_LICENSE-AGPL-3.0-or-later
├── LICENSE · NOTICE · CLA.md · CONTRIBUTORS.md · CONTRIBUTING.md
└── LICENSES/
```

---

## 3. The cardinal types (don't break these)

### `ZclAttribute` — 52 bytes exactly

Defined in `components/zap_common/include/zcl_attribute.h`. Shared by
**every consumer** in the project.

```cpp
struct ZclAttribute {
    char     key[20];     // z2m-style expose name (e.g. "state", "color_temp")
    uint8_t  val_type;    // VAL_BOOL / VAL_INT / VAL_UINT / VAL_STR
    uint16_t cluster;
    uint16_t attr_id;
    union { int32_t int_val; char str_val[24]; };
};
static_assert(sizeof(ZclAttribute) == 52, "ABI-critical");
```

### Other pinned sizes

- `ZclAttrEvent` = 96 B
- `ShadowAttr` = 52 B
- `ZapDevice` = 522 B

Any layout change requires bumping `NVS_SHADOW_VERSION` (currently v4)
and shipping a migration-or-wipe path.

---

## 4. Consuming these components

### From firmware (ESP Component Manager, preferred)

In `main/idf_component.yml`:

```yaml
dependencies:
  zap_common:
    git: "https://github.com/zhac-project/zhac-components.git"
    path: "components/zap_common"
    version: "^v2026051101"
  # ... repeat per component
```

Component Manager clones the repo once and resolves each
component from the `path:` inside that working copy.

### From local checkout (meta-repo build)

```bash
export IDF_COMPONENT_OVERRIDE_PATH=$PWD/../zhac-components/components
```

Component Manager prefers the override path before going to git.

### Standalone build / test

```bash
cd zhac-components
idf.py create-project-from-example _template   # if you want to exercise one
# or host-build components with unit tests:
cd components/device_shadow && cmake -S test -B build && cmake --build build
```

---

## 5. `zhc_adapter` — the library bridge

This is the **only** component that knows about `embedded-zhc`. Every
other firmware component goes through `zhc_adapter`'s API.

Exported surface (no direct `zhc::` includes elsewhere):

```cpp
bool zhac_adapter_try_decode(const ZclFrame&, ZclAttribute* out);
bool zhac_adapter_send_bool(uint64_t ieee, const char* key, bool v);
bool zhac_adapter_send_uint(uint64_t ieee, const char* key, uint32_t v);
bool zhac_adapter_send_string(uint64_t ieee, const char* key, const char* s);
bool zhac_adapter_has_def(uint64_t ieee);
bool zhac_adapter_configure(uint64_t ieee);            // bind + reports + initial reads
char* zhac_adapter_build_exposes_json(uint64_t ieee);  // caller frees
void zhac_adapter_invalidate_def_cache(uint64_t ieee); // after remove
void zhac_adapter_register_shadow_hook(...);           // callback injection
```

The shadow hook pattern is the canonical **"two components that would
need each other → use a hook instead"** solution the maintainer
prefers (persistent user preference — don't couple components
directly).

---

## 6. Licensing — mixed, per-file authoritative

Most components are Apache-2.0. A handful are AGPL-3.0-or-later
(currently none in this repo — AGPL components live in firmware repos
because they depend on firmware-specific code).

- **Authoritative licence = SPDX header in each file.** Repo-level
  `LICENSE` + per-component `LICENSE` are pointers.
- **arduinojson** is MIT and must stay MIT.
- When adding a new component, start from one of the
  `_TEMPLATE_LICENSE-*` files, fill in the SPDX header in every
  source file, and declare the licence in the component's own
  `LICENSE` file.

CLA: Apache ICLA v2.2 with §4 relicensing grant. Sign by adding
yourself to `CONTRIBUTORS.md` in your first PR in any ZHAC repo.

---

## 7. Conventions

- **C++17** on ESP-IDF (`-fno-exceptions -fno-rtti`).
- **Bounded queues** for anything the log path or ISR can hit.
  Unbounded `xQueueSend(...)` is a freeze waiting to happen — the
  S3 freeze tracked down in 2026-04 was
  `mqtt_gw_publish` blocking the `ESP_LOG` vprintf hook. Fix: worker
  task + bounded queue, drop-on-full, no logging inside the publish
  path.
- **Prefer callback hooks over mutual deps.** The maintainer
  explicitly prefers hook-registration (`zhac_adapter_register_shadow_hook`)
  over `zhac_adapter` and `device_shadow` including each other's
  headers.
- **`static_assert` on struct sizes** at the definition site.
- **No logging from hot paths.** `device_shadow` pipeline runs
  thousands of times a second under load; log only at transitions,
  not per-attribute.
- **Don't call `mqtt_gw_publish` from log output.** Ever. This is a
  permanent rule.

---

## 8. User preferences (persistent)

- **User builds firmware themselves.** Don't run `idf.py build` for
  them.
- **Early-dev stance.** Breaking changes are fine — no backwards-compat
  shims.
- **Prefer hook registration pattern** when two components would
  otherwise need each other.

---

## 9. Where to go next

- **ZCL attribute flow**: `components/device_shadow/README.md`.
- **HAP protocol**: `components/hap_protocol/README.md`,
  `hap_session/README.md`, `hap_json/README.md`.
- **Device library docs**: `embedded-zhc/README.md` (sibling repo).
- **API docs**: `zhac-docs/REST_API.md`, `WS_API.md`.
- **Component Manager docs** (Espressif): sibling `idf.py reconfigure`
  after any `idf_component.yml` change.

---

*Tag on first split: `v2026042301` · 2026-04-23.*
*Licensing: mixed (per-file SPDX authoritative) · Maintainer: Evgenij Cjura.*
