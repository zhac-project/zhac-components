# Contributing to zhac-components

This repo holds ten shared ESP-IDF components — two Apache-2.0, eight
AGPL-3.0-or-later. Per-file SPDX headers are the authoritative
license marker.

## License and CLA

Your contribution is submitted under the license of the **component
directory** you are editing:

- Inside `components/zap_common/` or `components/metrics/` → Apache-2.0.
- Anywhere else → AGPL-3.0-or-later.

Every contributor must sign `CLA.md` before their first patch lands.
See `CONTRIBUTORS.md` for the signup mechanism.

## Which component gets my change?

| Change type | Component |
|-------------|-----------|
| New attribute type, cluster constant, `ZclAttribute` layout | `zap_common` |
| New metric counter, histogram, exporter format | `metrics` |
| New HAP message type | `hap_protocol` + `hap_json` (encoder/decoder) |
| Shadow persistence / debounce / throttle | `device_shadow` |
| ZHC library ↔ firmware shim | `zhc_adapter` |
| Chip-specific component (P4-only or S3-only) | **NOT HERE** — goes in the respective firmware repo |

If your change touches `zap_common` or `metrics`, review whether it
should stay Apache-2.0-clean (portable) or whether the added dependency
pushes it into AGPL territory. When in doubt, open an issue before
coding.

## SPDX headers for new files

In `components/zap_common/` or `components/metrics/`:

```c
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
```

Everywhere else:

```c
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
```

## Style

- C++17 on ESP-IDF (no exceptions, no RTTI, no `std::string` /
  `std::vector` for persistent state).
- 4-space indent. `snake_case` for funcs/vars. `UPPER_CASE` for macros.
- `static_assert` on struct sizes for ABI stability — the ZclAttribute
  layout is relied on by both firmware cores and NVS persistence.
- Prefer callback-registration hooks over cross-component includes
  (see `device_shadow`'s shadow-hook pattern for reference).

## Local build + test

Each component ships a `test/` directory where applicable. Host tests:

```sh
cd components/zap_common/test
cmake -B build -S . && cmake --build build && ctest --test-dir build
```

Full firmware compile-check: you need to build from `zhac-main-core`
or `zhac-net-core` with this repo checked out as an override path:

```sh
export IDF_COMPONENT_OVERRIDE_PATH=$PWD/components
cd ../zhac-main-core  # or zhac-net-core
idf.py build
```

## NVS schema versioning

Any change to the on-disk layout of `ShadowAttr` (52 B today), `ZapDevice`
(522 B today), or any other NVS-persisted struct requires bumping the
schema version constant **in the same PR**. Missed bumps will brick
existing devices on upgrade. The relevant version macros:

- `device_shadow`: `NVS_SHADOW_VERSION`
- `zap_store`: `NVS_ZAP_STORE_VERSION` (if present)

## Reporting bugs

Open an issue with:
- Which component is involved
- Firmware + component versions (tag + SHA)
- Minimal reproduction
- Logs at `ESP_LOG_VERBOSE` if available
