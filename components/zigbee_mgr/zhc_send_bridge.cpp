// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Bridge between zhc_adapter (which owns ZCL frame encoding) and the
// ZNP radio. The adapter calls our `zhc_send_af` with a fully-formed
// ZCL frame (header + payload) plus destination addressing; we patch
// the TSN into the ZCL header and wrap in AF_DATA_REQUEST.
//
// Registered at startup by `zigbee_mgr_init` via
// `zhac_adapter_register_send`.

#include "zhc_adapter.h"
#include "zigbee_mgr.h"
#include "znp_driver.h"
#include "zcl_seq.h"   // zcl_seq_next()

#include "esp_log.h"
#include <cstdint>
#include <cstring>

static const char* TAG = "zhc_send";

extern "C" bool zhc_send_af(uint16_t nwk_addr, uint8_t dst_ep,
                             uint16_t cluster_id,
                             const uint8_t* zcl_data, size_t zcl_len) {
    if (!zcl_data || zcl_len < 3 || zcl_len > 200) return false;

    // Patch TSN into the ZCL header (byte 1). Encoder writes 0x00 as a
    // placeholder; we own the global sequence so responses correlate
    // with requests on the same side as `zigbee_zcl_on_off` does.
    uint8_t zcl_local[200];
    std::memcpy(zcl_local, zcl_data, zcl_len);
    const uint8_t seq = zcl_seq_next();
    // Header shape depends on manufacturer-specific bit (fc & 0x04).
    // Standard frames: [fc][tsn][cmd] — tsn at byte 1.
    // Manu-specific:   [fc][mc:2][tsn][cmd] — tsn at byte 3.
    const bool manu_specific = (zcl_local[0] & 0x04) != 0;
    if (manu_specific && zcl_len < 5) return false;
    zcl_local[manu_specific ? 3 : 1] = seq;

    // AF_DATA_REQUEST payload (Z-Stack 3.x):
    // [dst_addr 2B LE, dst_ep, src_ep=1, cluster 2B LE,
    //  trans_id, options, radius, len, data...]
    uint8_t af_pl[10 + 200];
    af_pl[0] = nwk_addr & 0xFF;
    af_pl[1] = (nwk_addr >> 8) & 0xFF;
    af_pl[2] = dst_ep;
    af_pl[3] = 0x01;                        // src_ep
    af_pl[4] = cluster_id & 0xFF;
    af_pl[5] = (cluster_id >> 8) & 0xFF;
    af_pl[6] = seq;                         // trans_id — share ZCL seq
    af_pl[7] = 0x00;                        // options
    af_pl[8] = 0x0F;                        // radius
    af_pl[9] = static_cast<uint8_t>(zcl_len);
    std::memcpy(af_pl + 10, zcl_local, zcl_len);

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;   // AF_DATA_REQUEST
    req.payload     = af_pl;
    req.payload_len = 10 + zcl_len;

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 3)) {
        ESP_LOGE(TAG, "AF_DATA_REQUEST no SRSP nwk=0x%04x cl=0x%04x",
                 nwk_addr, cluster_id);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "AF_DATA_REQUEST status=0x%02x nwk=0x%04x cl=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF,
                 nwk_addr, cluster_id);
        return false;
    }
    ESP_LOGI(TAG, "zhc send nwk=0x%04x ep=%u cl=0x%04x len=%u seq=%u",
             nwk_addr, dst_ep, cluster_id,
             static_cast<unsigned>(zcl_len), seq);
    return true;
}

extern "C" void zhc_send_bridge_register(void) {
    zhac_adapter_register_send(&zhc_send_af);
}
