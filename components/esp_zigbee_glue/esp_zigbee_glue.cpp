// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_glue — see esp_zigbee_glue.h. Only compiled with meaning when the
// esp-zigbee backend is selected; otherwise a linkable no-op (the #else at the
// bottom) so a firmware may always call esp_zigbee_backend_register().
//
// This translation unit BINDS to esp-zigbee-lib and so only compiles inside a
// firmware IDF build for the S31/S3 host (with the SDK pulled by the
// config-gated idf_component.yml). It is not host-buildable; the pure logic it
// leans on (esp_zigbee_backend's mapper/builder) is host-tested separately (P0).
// Verified end-to-end on the ESP Thread BR board — design doc §6.2 (HW gate).

#include "sdkconfig.h"

#if defined(CONFIG_ZHAC_ZIGBEE_BACKEND_ESP_ZIGBEE)

#include "esp_zigbee_glue.h"
#include "esp_zigbee_backend.h"   // P0 pure core: zhac::esp_zigbee::*
#include "zhc_adapter.h"
#include "esp_log.h"

#include <cstdint>
#include <cstddef>

extern "C" {
#include "ezbee/aps.h"   // ezb_apsde_data_*, ezb_address_t, EZB_APS_ADDR_MODE_*, EZB_APSDE_TX_OPT_*
#include "ezbee/nwk.h"   // ezb_nwk_get_short_address (P2 addressing)
}

using namespace zhac::esp_zigbee;

namespace {

const char* TAG = "esp_zb_glue";

// RX seam: an incoming APSDE-DATA.indication → the zhc_adapter decode path.
// Returns true so the stack treats the frame as consumed — ZHAC's device
// library owns all ZCL decode (the z2m-style "dumb stack, host decodes" model).
bool on_apsde_ind(const ezb_apsde_data_ind_t* ind) {
    if (ind == nullptr) return false;

    // ezb indication → neutral (exact field copy; the only SDK-typed step).
    EspZbApsInd in{};
    in.status       = ind->status;
    in.src_nwk      = ind->src_address.u.short_addr;
    in.src_endpoint = ind->src_endpoint;
    in.cluster_id   = ind->cluster_id;
    in.profile_id   = ind->profile_id;
    in.dst_is_group = (ind->dst_address.addr_mode == EZB_ADDR_MODE_GROUP);
    in.dst_group    = in.dst_is_group ? ind->dst_address.u.group_addr.group : 0;
    in.lqi          = ind->lqi;
    in.asdu         = ind->asdu;
    in.asdu_len     = ind->asdu_length;

    EspZbDecodeArgs a{};
    if (!esp_zb_map_incoming(in, a)) {
        return false;   // dropped (bad status / empty / oversize) — leave to stack
    }

    // ieee / model / manufacturer come from the device pool (by short address),
    // not the frame — the same source znp_driver feeds zhac_adapter_try_decode.
    // TODO(P2): resolve via zigbee_mgr's pool (nwk → ieee + Basic model/mfg) and
    // keep the runtime addr map fresh on device-announce. Until P2 lands, decode
    // with an unresolved ieee (the def match still works for cluster-keyed
    // devices; Tuya/lumi mfg discrimination needs the pool lookup).
    uint64_t    ieee  = 0;         // TODO(P2): pool_find_by_nwk(in.src_nwk)
    const char* model = nullptr;  // TODO(P2)
    const char* mfg   = nullptr;  // TODO(P2)

    zhac_adapter_set_runtime_addr(ieee, in.src_nwk);
    zhac_adapter_try_decode(ieee, model, mfg, a.group_id, a.cluster_id,
                            a.src_endpoint, a.linkquality, a.zcl, a.zcl_len);
    return true;
}

// TX seam: zhc_adapter's registered send hook → an APSDE-DATA.request.
// `zcl_data` is already a fully-framed ZCL body (TSN patched by the encoder).
bool af_send(uint16_t nwk_addr, uint8_t dst_ep, uint16_t cluster_id,
             const uint8_t* zcl_data, size_t zcl_len) {
    EspZbApsReq r{};
    if (!esp_zb_build_outgoing(nwk_addr, dst_ep, cluster_id, zcl_data, zcl_len, r)) {
        return false;
    }

    // neutral → ezb request (exact field copy; the only SDK-typed step).
    ezb_apsde_data_req_t req{};
    req.dst_address.addr_mode    = EZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.dst_address.u.short_addr = r.dst_nwk;
    req.src_endpoint             = r.src_endpoint;
    req.dst_endpoint             = r.dst_endpoint;
    req.cluster_id               = r.cluster_id;
    req.profile_id               = r.profile_id;
    req.radius                   = r.radius;
    req.tx_options               = EZB_APSDE_TX_OPT_ACK_TX;
    req.asdu                     = const_cast<uint8_t*>(r.asdu);
    req.asdu_length              = static_cast<uint16_t>(r.asdu_len);

    return ezb_apsde_data_request(&req) == EZB_ERR_NONE;
}

}  // namespace

extern "C" bool esp_zigbee_backend_register(void) {
    // The RX + TX seams — the P0-tested translation, live against the SDK.
    ezb_apsde_data_indication_handler_register(on_apsde_ind);
    zhac_adapter_register_send(af_send);

    // TODO(P2) — the lifecycle wiring, all mirroring proven references:
    //  * device_backend_register(&s_backend) for command/control
    //    (shape: zhac-main-core/components/zigbee_backend/zigbee_backend.cpp);
    //  * zhac_adapter_register_configure(bind, report) → ezb ZDO bind +
    //    ConfigureReporting;
    //  * ezb_app_signal_add_handler(...) → DEVICE_ANNCE/LEAVE → zigbee_mgr, and
    //    ZDO interview (active-ep / simple-desc / Basic reads);
    //  * coordinator bring-up: ezb device-type COORDINATOR + start_stack in
    //    UART_RCP radio mode (sequence: esp-znp-core/components/znp_ezb/znp_ezb.c;
    //    RCP UART pins from board Kconfig).
    ESP_LOGI(TAG, "esp-zigbee backend: RX/TX seams registered (P1)");
    return true;
}

#else  // !CONFIG_ZHAC_ZIGBEE_BACKEND_ESP_ZIGBEE

#include "esp_zigbee_glue.h"
// Linkable no-op so a firmware can reference esp_zigbee_backend_register()
// unconditionally; it only does anything when the backend is selected.
extern "C" bool esp_zigbee_backend_register(void) { return true; }

#endif
