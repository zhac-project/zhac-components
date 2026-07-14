// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// esp_zigbee_backend — pure RX mapper / TX builder (P0). See the header for
// the design.

#include "esp_zigbee_backend.h"

namespace zhac::esp_zigbee {

bool esp_zb_map_incoming(const EspZbApsInd& in, EspZbDecodeArgs& out) {
    // Drop anything the stack flagged, and any frame that can't carry a ZCL
    // body — fail closed rather than hand a bad frame to the decoder.
    if (in.status != 0) return false;
    if (in.asdu == nullptr || in.asdu_len == 0 || in.asdu_len > kAsduMax) {
        return false;
    }
    // A groupcast's destination group address becomes group_id; a unicast is 0
    // (same convention znp_driver uses when it fills AfRawFrame::group_id).
    out.group_id     = in.dst_is_group ? in.dst_group : 0;
    out.cluster_id   = in.cluster_id;
    out.src_endpoint = in.src_endpoint;
    out.linkquality  = in.lqi;
    out.zcl          = in.asdu;   // no copy — the ASDU IS the ZCL body
    out.zcl_len      = in.asdu_len;
    return true;
}

bool esp_zb_build_outgoing(uint16_t nwk_addr, uint8_t dst_endpoint,
                           uint16_t cluster_id, const uint8_t* zcl,
                           size_t zcl_len, EspZbApsReq& out) {
    if (zcl == nullptr || zcl_len == 0 || zcl_len > kAsduMax) return false;
    out.dst_nwk      = nwk_addr;
    out.dst_endpoint = dst_endpoint;
    out.src_endpoint = kCoordEndpoint;   // frames originate from the coordinator EP
    out.profile_id   = kHaProfileId;
    out.cluster_id   = cluster_id;
    out.tx_options   = kTxOptAckReq;     // request an APS ACK (matches znp send)
    out.radius       = 0;                // stack default
    out.asdu         = zcl;              // the send hook already ZCL-framed it
    out.asdu_len     = zcl_len;
    return true;
}

}  // namespace zhac::esp_zigbee
