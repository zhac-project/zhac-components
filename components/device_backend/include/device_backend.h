// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/device_backend/include/device_backend.h
// Protocol-agnostic device backend interface + registry.
// Each protocol (Zigbee, BLE, Thread, ...) implements one DeviceBackend
// and registers it at startup. Higher layers dispatch through find()/find_by_ieee().
#pragma once
#include "zap_common.h"
#include <cstdint>

static constexpr uint8_t DEVICE_BACKEND_MAX = 4;

struct DeviceBackend {
    NcpProtocol protocol;
    const char* name;           // "Zigbee", "BLE", "Thread"

    // Lifecycle
    bool   (*init)(void);
    bool   (*poll)(void);       // called from protocol task; drives UART/transport
    bool   (*is_running)(void);

    // Device discovery
    bool   (*start_discovery)(uint8_t duration_s);  // permit-join / BLE scan
    bool   (*stop_discovery)(void);
    bool   (*interview)(uint64_t ieee, uint16_t addr_hint);

    // Attribute I/O (key-based — semantic dispatch).
    // `ep` = endpoint; 0 means "use device default / first endpoint". Multi-
    // endpoint devices (dual switches, multi-sensor plugs) previously always
    // had commands routed to endpoints[0]; callers should now pass the
    // specific endpoint from the request (QWEN §11).
    bool   (*write_attr)(uint64_t ieee, uint8_t ep, const char* key, int32_t val);
    bool   (*read_attr)(uint64_t ieee, uint8_t ep, const char* key);

    // Device management
    bool   (*get_device_list)(ZapDevice* out, uint16_t max, uint16_t* count_out);
    bool   (*get_device)(uint64_t ieee, ZapDevice* out);
    bool   (*remove_device)(uint64_t ieee);
    bool   (*rename_device)(uint64_t ieee, const char* name);
};

// Registry — call at startup from each backend
bool device_backend_register(DeviceBackend* b);

// Lookup by protocol type
DeviceBackend* device_backend_find(NcpProtocol proto);

// Lookup by IEEE — iterates backends, asks each for the device
DeviceBackend* device_backend_find_by_ieee(uint64_t ieee);

// Iterate all registered backends (returns count)
uint8_t device_backend_count();
DeviceBackend* device_backend_get(uint8_t index);
