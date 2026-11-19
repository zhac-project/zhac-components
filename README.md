# zhac-components

Shared ESP-IDF components of the ZHAC hub firmware, [zhac-wired-core]
(ESP32-S31, or ESP32-P4 with an ESP32-C6 radio). `ws_server`, `remote_client`,
`lua_engine` and `lua_cjson` came here, with their history, from the archived
two-chip repos (zhac-net-core, zhac-main-core) in November 2026.

[zhac-wired-core]: https://github.com/zhac-project/zhac-wired-core

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
| `ntp_cfg`       | The hub's time server: the configured host (NVS `sys_cfg/ntp_server`) or the public default, and starting or restarting SNTP with it. | AGPL-3.0-or-later |
| `device_cmd`    | The one device-command path (set attribute, rename, permit join, remove): pool snapshot under the lock, bool / integer / decimal / string to the converter, optimistic shadow mirror, one set of result words. Every transport calls it. | AGPL-3.0-or-later |
| `ha_bridge`     | Home Assistant MQTT discovery: discovery configs from device exposes, per-attribute state topics, `…/set` commands. Pure builder host-tested. | AGPL-3.0-or-later |
| `ws_server`     | The web server's WebSocket endpoint: auth, per-client send queue, broadcast to authed clients. Host-tested. | AGPL-3.0-or-later |
| `remote_client` | The hub's outbound link to zhac-cloud: auth frame, reconnect backoff, the allow-list of commands the cloud may send. Host-tested. | AGPL-3.0-or-later |
| `lua_engine`    | Lua 5.5 scripting: sandbox, allocator caps, scheduler, `zhac.*` module, script store. Needs `georgik/lua` (declared in its `idf_component.yml`). Host-tested. | AGPL-3.0-or-later |
| `lua_cjson`     | Vendored openresty/lua-cjson 2.1.0.14 for Lua's `require "cjson"`. | MIT (vendored) |
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

### Via local path

zhac-wired-core finds a sibling checkout by itself (`../zhac-components/components`);
`IDF_COMPONENT_OVERRIDE_PATH` points it anywhere else:

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

Releases tagged `vYYYYMMDDVV` (date + two-digit revision). zhac-wired-core's
`release-manifest.json` pins the commit each firmware release builds against.

## Contributing

See `CONTRIBUTING.md`. All contributions require signing `CLA.md`.
