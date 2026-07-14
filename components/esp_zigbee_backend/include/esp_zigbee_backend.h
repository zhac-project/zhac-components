// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend — native esp-zigbee-lib radio backend for ZHAC (P0 core).
//
// Design: extra/docs/ESP_ZIGBEE_BACKEND_DESIGN.md. This is the *pure,
// host-testable* seam layer (P0): the RX mapper and TX builder that translate
// between an incoming/outgoing APS frame and the `zhc_adapter` contract. It
// depends on NOTHING from esp-zigbee-lib — it operates on ZHAC-owned neutral
// PODs. The P1 glue does the trivial `ezb_apsde_data_ind_t <-> EspZbApsInd` /
// `EspZbApsReq -> ezb_apsde_data_req_t` field copy at the SDK boundary, so the
// tested core never binds to the SDK's exact struct layout.
//
// RX flow:  radio -> ezb ind --(glue copy)--> EspZbApsInd --esp_zb_map_incoming-->
//           EspZbDecodeArgs --(glue)--> zhac_adapter_try_decode(...)
// TX flow:  zhac_af_send_fn_t(nwk,ep,cluster,zcl,len) --esp_zb_build_outgoing-->
//           EspZbApsReq --(glue copy)--> ezb_apsde_data_request(...)

#pragma once

#include <cstddef>
#include <cstdint>

namespace zhac::esp_zigbee {

// Max APS ASDU (ZCL body) we carry. Matches the ZCL payload ceiling used
// elsewhere in the stack; frames longer than this are rejected rather than
// truncated. Reconcile against esp-zigbee-lib's fragmentation limit at P1.
inline constexpr size_t   kAsduMax        = 250;
// ZHAC coordinator application endpoint (source EP for host-originated frames).
inline constexpr uint8_t  kCoordEndpoint  = 1;
// Home Automation profile.
inline constexpr uint16_t kHaProfileId    = 0x0104;
// APS TX option: request an end-to-end APS ACK. Neutral bit; the glue maps it
// to the SDK's EZB_APSDE_TX_OPT_ACK_TX when filling the real request.
inline constexpr uint8_t  kTxOptAckReq    = 0x04;

// Neutral mirror of an incoming APSDE-DATA.indication. Populated by the glue
// from the SDK's ezb_apsde_data_ind_t.
struct EspZbApsInd {
    uint8_t        status;        // 0 == OK; non-zero indication is dropped
    uint16_t       src_nwk;       // source short address
    uint8_t        src_endpoint;
    uint16_t       cluster_id;
    uint16_t       profile_id;
    bool           dst_is_group;  // addr_mode == group
    uint16_t       dst_group;     // group address when dst_is_group
    uint8_t        lqi;           // link quality
    const uint8_t* asdu;          // ZCL body (byte 0 = frame control)
    size_t         asdu_len;
};

// The exact argument set `zhac_adapter_try_decode` consumes from a frame.
// ieee / model_id / manufacturer_name are NOT here — the glue supplies those
// from the device pool, not from the frame.
struct EspZbDecodeArgs {
    uint16_t       group_id;      // 0 for unicast, else the group address
    uint16_t       cluster_id;
    uint8_t        src_endpoint;
    uint8_t        linkquality;
    const uint8_t* zcl;           // == ind.asdu
    size_t         zcl_len;
};

// Neutral mirror of an outgoing APSDE-DATA.request. Filled by the builder;
// the glue copies it into the SDK's ezb_apsde_data_req_t.
struct EspZbApsReq {
    uint16_t       dst_nwk;
    uint8_t        dst_endpoint;
    uint8_t        src_endpoint;
    uint16_t       profile_id;
    uint16_t       cluster_id;
    uint8_t        tx_options;
    uint8_t        radius;
    const uint8_t* asdu;          // == the ZCL frame handed to the send hook
    size_t         asdu_len;
};

// RX mapper: extract the `zhac_adapter_try_decode` inputs from an indication.
// Returns false (and leaves *out unspecified) when the frame must be dropped:
// non-OK status, null/empty ASDU, or ASDU longer than kAsduMax. group_id is
// the dst group address for a groupcast, 0 for a unicast.
bool esp_zb_map_incoming(const EspZbApsInd& in, EspZbDecodeArgs& out);

// TX builder: turn the `zhac_af_send_fn_t(nwk, dst_ep, cluster, zcl, len)`
// arguments into a neutral APS request (unicast, HA profile, ACK requested,
// coordinator source endpoint). Returns false for a null/empty ZCL frame or
// one longer than kAsduMax.
bool esp_zb_build_outgoing(uint16_t nwk_addr, uint8_t dst_endpoint,
                           uint16_t cluster_id, const uint8_t* zcl,
                           size_t zcl_len, EspZbApsReq& out);

}  // namespace zhac::esp_zigbee
