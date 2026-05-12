// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Bridge between zhc_adapter's configure hooks and ZNP ZDO/ZCL
// transport. `zhac_adapter_configure(ieee, model, manu)` at device
// join walks the PreparedDefinition's bindings[] + reports[] arrays
// and fires one of these bridge fns per entry.
//
// v1 covers bind (ZDO_BIND_REQ via the existing helper). Reporting
// configuration is stubbed as a no-op for now — most sleepy
// end-devices in our registry ship with sensible defaults or rely on
// host-side polling (mcuSyncTime etc.).

#include "zhc_adapter.h"
#include "zigbee_mgr.h"
#include "zigbee_pool.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

static const char* TAG = "zhc_cfg";

extern "C" bool zhc_cfg_bind_af(uint64_t ieee, uint8_t ep,
                                 uint16_t cluster) {
    const ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) {
        ESP_LOGW(TAG, "bind: unknown ieee=0x%016llx",
                 static_cast<unsigned long long>(ieee));
        return false;
    }
    const uint64_t coord = zigbee_mgr_coordinator_ieee();
    if (coord == 0) {
        ESP_LOGW(TAG, "bind: coordinator ieee not yet known");
        return false;
    }
    return zigbee_zdo_bind(dev->nwk_addr, ieee, ep, cluster, coord, 1);
}

extern "C" bool zhc_cfg_report_af(uint64_t /*ieee*/, uint8_t /*ep*/,
                                    uint16_t cluster,
                                    uint16_t /*attr_id*/,
                                    uint8_t  /*attr_type*/,
                                    uint16_t /*min_interval*/,
                                    uint16_t /*max_interval*/,
                                    uint32_t /*reportable_change*/,
                                    uint16_t /*manu_code*/) {
    // v1: log + accept. Wire the real ZCL configureReporting frame
    // here when we have a device that needs it (thermostats, metering
    // plugs that don't auto-report).
    ESP_LOGD(TAG, "report (stub): cluster=0x%04x", cluster);
    return true;
}

// ── Declarative step transports ─────────────────────────────────────
//
// Bridge callbacks for the config_steps pipeline. Each wraps the
// ZCL helper in zcl_commands.cpp so vendor cpps can stay platform-
// agnostic (pure data + indices).

extern "C" bool zhc_cfg_cmd_af(uint64_t /*ieee*/, uint16_t nwk,
                                uint8_t endpoint, uint16_t cluster,
                                uint8_t cmd_id,
                                const uint8_t* payload,
                                uint8_t payload_len,
                                uint8_t flags) {
    // Config-step Cmds block on AF_DATA_CONFIRM so MAC-level delivery
    // failures (ZMacTransactionExpired = 0xF0) surface as a false
    // return — the step walker then aborts the pipeline instead of
    // silently marking "configure DONE" on a device that never
    // actually received the magic packet / setZones frame. Budget
    // 2.5 s; observed confirms land ~100-500 ms post-TX.
    return zigbee_zcl_cluster_command_wait_confirm(
        nwk, endpoint, cluster, cmd_id, payload, payload_len, flags, 2500);
}

extern "C" bool zhc_cfg_read_af(uint64_t /*ieee*/, uint16_t nwk,
                                 uint8_t endpoint, uint16_t cluster,
                                 const uint8_t* attr_ids_le,
                                 uint8_t attr_count,
                                 uint16_t manu_code) {
    return zigbee_zcl_read(nwk, endpoint, cluster, attr_ids_le, attr_count,
                            manu_code);
}

extern "C" void zhc_cfg_sleep(uint16_t wait_ms) {
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

extern "C" void zhc_configure_bridge_register(void) {
    zhac_adapter_register_configure(&zhc_cfg_bind_af, &zhc_cfg_report_af);
    zhac_adapter_register_configure_ex(&zhc_cfg_cmd_af,
                                         &zhc_cfg_read_af,
                                         &zhc_cfg_sleep);
}
