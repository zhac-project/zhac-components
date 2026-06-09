// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Bridge between zhc_adapter's configure hooks and ZNP ZDO/ZCL
// transport. `zhac_adapter_configure(ieee, model, manu)` at device
// join walks the PreparedDefinition's bindings[] + reports[] arrays
// and fires one of these bridge fns per entry.
//
// Bind goes through ZDO_BIND_REQ; report goes through ZCL Configure
// Reporting (cmd 0x06); cmd/read use the matching ZCL helpers in
// zcl_commands.cpp. Battery sensors usually self-report on a vendor
// cadence so reporting setup is harmless when redundant; mains-powered
// devices (thermostats, metering plugs) depend on it.

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
    // F6/F35 (FINDINGS.md): snapshot nwk_addr under the advisory lock, then
    // make the blocking ZDO call with the local copy — never hold a raw
    // pool pointer across a swap-with-last-capable window.
    uint16_t nwk = 0;
    bool found = false;
    zigbee_pool_lock();
    if (const ZapDevice* dev = pool_find_by_ieee(ieee)) { nwk = dev->nwk_addr; found = true; }
    zigbee_pool_unlock();
    if (!found) {
        ESP_LOGW(TAG, "bind: unknown ieee=0x%016llx",
                 static_cast<unsigned long long>(ieee));
        return false;
    }
    const uint64_t coord = zigbee_mgr_coordinator_ieee();
    if (coord == 0) {
        ESP_LOGW(TAG, "bind: coordinator ieee not yet known");
        return false;
    }
    return zigbee_zdo_bind(nwk, ieee, ep, cluster, coord, 1);
}

extern "C" bool zhc_cfg_report_af(uint64_t ieee, uint8_t ep,
                                    uint16_t cluster,
                                    uint16_t attr_id,
                                    uint8_t  attr_type,
                                    uint16_t min_interval,
                                    uint16_t max_interval,
                                    uint32_t reportable_change,
                                    uint16_t manu_code) {
    // Build + ship a ZCL Configure Reporting frame for ONE attribute.
    // Previously this was a no-op stub which made every def's `reports[]`
    // silently inert. zhc_cfg_bind_af above resolves nwk from the pool
    // the same way — reusing the lookup keeps the IEEE→nwk decoupling
    // out of the device cpps.
    // F6/F35 (FINDINGS.md): snapshot nwk_addr under the advisory lock.
    uint16_t nwk = 0;
    bool found = false;
    zigbee_pool_lock();
    if (const ZapDevice* dev = pool_find_by_ieee(ieee)) { nwk = dev->nwk_addr; found = true; }
    zigbee_pool_unlock();
    if (!found) {
        ESP_LOGW(TAG, "report: unknown ieee=0x%016llx",
                 static_cast<unsigned long long>(ieee));
        return false;
    }
    return zigbee_zcl_configure_report(nwk, ep, cluster,
                                        attr_id, attr_type,
                                        min_interval, max_interval,
                                        reportable_change, manu_code);
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

extern "C" bool zhc_cfg_write_af(uint64_t /*ieee*/, uint16_t nwk,
                                  uint8_t endpoint, uint16_t cluster,
                                  uint16_t attr, uint8_t type,
                                  const uint8_t* val, uint8_t len,
                                  uint16_t manu) {
    // Forward the manufacturer code end-to-end. When `manu` != 0,
    // zigbee_zcl_write_attr builds a manu-specific ZCL frame (FC=0x04,
    // 5-byte header) — required for lumi 0xFCC0 attribute writes, which
    // z2m emits as `endpoint.write("manuSpecificLumi", {...},
    // {manufacturerCode: 0x115f})` and which Aqara hardware silently
    // rejects if sent profile-wide. (The earlier claim that 0xFCC0 is
    // fine profile-wide because "the cluster id carries the specificity"
    // was wrong — contradicted by the lib's own def/test which thread
    // manu_code=0x115F through to here.) `manu == 0` keeps the legacy
    // profile-wide write (Tuya 0x8004, IAS CIE) unchanged.
    return zigbee_zcl_write_attr(nwk, endpoint, cluster, attr, type, val, len,
                                  manu);
}

extern "C" void zhc_cfg_sleep(uint16_t wait_ms) {
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

extern "C" void zhc_configure_bridge_register(void) {
    zhac_adapter_register_configure(&zhc_cfg_bind_af, &zhc_cfg_report_af);
    zhac_adapter_register_configure_ex(&zhc_cfg_cmd_af,
                                         &zhc_cfg_read_af,
                                         &zhc_cfg_sleep);
    zhac_adapter_register_configure_write(&zhc_cfg_write_af);
}
