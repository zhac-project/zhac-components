# zhac-components

Shared ESP-IDF components consumed by both ZHAC firmware cores
([zhac-main-core] on ESP32-P4, [zhac-net-core] on ESP32-S3). Ten
first-party components plus one vendored third-party dependency.

[zhac-main-core]: https://github.com/zhac-project/zhac-main-core
[zhac-net-core]:  https://github.com/zhac-project/zhac-net-core

Per-component `LICENSE` files and SPDX headers are authoritative.

## Component inventory

| Component | Purpose | License |
|-----------|---------|---------|
| `zap_common`    | Shared types — `ZclAttribute` (52 B), `ValType`, cluster IDs. Every other component depends on this. | Apache-2.0 |
| `metrics`       | X-macro metric registry — counters, timers, gauges. Portable. | Apache-2.0 |
| `event_bus`     | Lightweight publish/subscribe bus on FreeRTOS queues. | AGPL-3.0-or-later |
| `hap_protocol`  | S3↔P4 wire format — message IDs, CRC, headers. | AGPL-3.0-or-later |
| `hap_session`   | HAP session + retry/queue layer. | AGPL-3.0-or-later |
| `hap_json`      | JSON encode/decode for HAP payloads (depends on `arduinojson`). | AGPL-3.0-or-later |
| `device_shadow` | NVS-backed attribute cache (string-keyed, debounce/throttle). | AGPL-3.0-or-later |
| `device_backend`| Backend abstraction — unifies `zigbee_backend` + future backends. | AGPL-3.0-or-later |
| `zap_store`     | NVS key/value store used by shadow + rule store. | AGPL-3.0-or-later |
| `zhc_adapter`   | One-way bridge from the `embedded-zhc` library into firmware components. | AGPL-3.0-or-later |
| `arduinojson`   | Vendored MIT header-only JSON library — upstream bblanchon/ArduinoJson. Used by `hap_json`. | MIT (vendored) |

## Consuming these components

### Via ESP Component Manager (recommended)

In your firmware's `main/idf_component.yml`:

```yaml
dependencies:
  zap_common:
    git: "https://github.com/zhac-project/zhac-components.git"
    path: "components/zap_common"
    version: "^v2026051101"
  # ... repeat for each component you need
```

### Via local path (for meta-repo builds)

If you're building from `zhac-platform`, its `justfile` sets
`IDF_COMPONENT_OVERRIDE_PATH` so the manager uses the local checkout
instead of fetching:

```sh
export IDF_COMPONENT_OVERRIDE_PATH=$PWD/../zhac-components/components
idf.py build
```

## Building / testing this repo standalone

Most components are ESP-IDF-flavoured and cannot be built on host.
CI runs a matrix of compile-only builds against ESP-IDF v6.0 on both
`esp32p4` and `esp32s3` targets. See `.github/workflows/ci.yml`.

The `zap_common` + `metrics` modules have host-side unit tests:

```sh
cd components/zap_common/test
cmake -B build -S . && cmake --build build && ctest --test-dir build
```

## Licensing — mixed, per-file authoritative

- **zap_common / metrics** are Apache-2.0. They are portable utilities
  reused by `embedded-zhc` (also Apache) and should stay reusable
  outside ZHAC.
- **`arduinojson`** retains its upstream **MIT** license — vendored
  copy of bblanchon/ArduinoJson. See `components/arduinojson/LICENSE`.
- **All other components** are AGPL-3.0-or-later. They are
  firmware-specific, tied to ZHAC's architecture.

Combining Apache-2.0 + MIT + AGPL-3.0 code in a single firmware build is
legal: the combined work is distributed under AGPL-3.0-or-later; the
Apache-2.0 / MIT parts remain under their own licenses when used in
isolation. See `LICENSE` for the umbrella overview and `NOTICE` for
third-party attribution.

## Versioning

Releases tagged `vYYYYMMDDVV`. See `zhac-platform` README for the
scheme.

## Contributing

See `CONTRIBUTING.md`. All contributions require signing `CLA.md`.
