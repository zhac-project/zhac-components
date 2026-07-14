// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_glue — SDK-binding half of the esp-zigbee backend (P1). Wires the
// pure esp_zigbee_backend core (P0) to esp-zigbee-lib + zhc_adapter. See
// extra/docs/ESP_ZIGBEE_BACKEND_DESIGN.md.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register the esp-zigbee backend with zhc_adapter and (P2) device_backend, and
// bring up the coordinator. Called once at boot by a firmware built with
// CONFIG_ZHAC_ZIGBEE_BACKEND_ESP_ZIGBEE selected. Naming mirrors the existing
// zigbee_backend_register() / device_backend_register() pattern. Returns true
// on success. When the backend is NOT selected this is a linkable no-op
// returning true (so a firmware may reference it unconditionally).
bool esp_zigbee_backend_register(void);

#ifdef __cplusplus
}
#endif
