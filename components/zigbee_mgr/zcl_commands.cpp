// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zcl_commands.cpp
#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "znp_driver.h"
#include "znp_confirm.h"
#include "zcl_seq.h"   // zcl_seq_next()
#include "zhc_adapter.h"
#include "esp_log.h"
#include <cstring>
#include <ctime>

static const char* TAG = "zcl_commands";

bool zigbee_zcl_on_off(uint16_t nwk_addr, uint8_t ep, uint8_t cmd) {
    // ZCL frame: [frame_ctrl=0x01, seq, cmd_id=cmd]. Seq is the global ZCL
    // counter (zcl_seq_next) shared with the converter layer so responses
    // correlate correctly — a hardcoded 0x01 made every command look like
    // the same transaction to some devices (CODEX §6).
    uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[3] = {0x01, seq, cmd};

    // AF_DATA_REQUEST payload (Z-Stack 3.x format):
    // [dst_addr 2B LE, dst_ep, src_ep=1, cluster_id 2B LE,
    //  trans_id, options, radius, len, data...]
    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = nwk_addr & 0xFF;
    af_pl[1] = (nwk_addr >> 8) & 0xFF;
    af_pl[2] = ep;          // dst_ep
    af_pl[3] = 0x01;        // src_ep
    af_pl[4] = 0x06; af_pl[5] = 0x00;  // cluster 0x0006 LE
    af_pl[6] = seq;         // trans_id (shared with ZCL seq)
    af_pl[7] = 0x00;        // options
    af_pl[8] = 0x0F;        // radius (15 hops max)
    af_pl[9] = sizeof(zcl_frame);       // len
    for (size_t i = 0; i < sizeof(zcl_frame); i++)
        af_pl[10 + i] = zcl_frame[i];

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;   // AF_DATA_REQUEST
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 3)) {
        ESP_LOGE(TAG, "AF_DATA_REQUEST: no SRSP");
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "AF_DATA_REQUEST: status=0x%02x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    ESP_LOGI(TAG, "ZCL On/Off cmd=0x%02x → nwk=0x%04x ep=%d", cmd, nwk_addr, ep);
    return true;
}

bool zigbee_permit_join(uint8_t duration_s) {
    // ZDO_MGMT_PERMIT_JOIN_REQ payload:
    //   AddrMode(1) + DstAddr(2 LE) + Duration(1) + TCSignificance(1) = 5 bytes
    // AddrMode 0x0F = broadcast to all devices (routers + coordinator).
    uint8_t pl[5] = {
        0x0F,          // AddrMode = broadcast
        0xFC, 0xFF,    // DstAddr = 0xFFFC LE (broadcast to routers + coord)
        duration_s,    // duration (0=close, 255=open indefinitely)
        0x01,          // tc_significance — MUST be 1 per Zigbee §2.4.3.3.5
                       // so the Trust Center (this coord) also enters
                       // its join-accepting window. With 0 only routers
                       // forward the broadcast and ZB3 end-devices like
                       // MiBoxer FUT089Z fail to see a joinable network.
    };
    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x36;  // ZDO_MGMT_PERMIT_JOIN_REQ
    req.payload = pl; req.payload_len = sizeof(pl);
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 3)) {
        ESP_LOGE(TAG, "PERMIT_JOIN: no SRSP");
        return false;
    }
    if (rsp.payload_len >= 1 && rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "PERMIT_JOIN: status=0x%02x", rsp.payload[0]);
        return false;
    }
    ESP_LOGI(TAG, "permit_join duration=%ds", duration_s);
    return true;
}

static bool zdo_bind_unbind(uint8_t cmd1_byte,
                             uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                             uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    // ZDO_BIND_REQ / ZDO_UNBIND_REQ payload (Z-Stack format):
    // [dst_addr 2B LE][src_ieee 8B LE][src_ep 1B][cluster 2B LE]
    // [addr_mode=0x03 1B][dst_ieee 8B LE][dst_ep 1B]  = 23 bytes
    uint8_t pl[23];
    pl[0] = src_nwk & 0xFF; pl[1] = (src_nwk >> 8) & 0xFF;
    for (int i = 0; i < 8; i++) pl[2 + i] = (src_ieee >> (i * 8)) & 0xFF;
    pl[10] = src_ep;
    pl[11] = cluster & 0xFF; pl[12] = (cluster >> 8) & 0xFF;
    pl[13] = 0x03;  // addr_mode = IEEE
    for (int i = 0; i < 8; i++) pl[14 + i] = (dst_ieee >> (i * 8)) & 0xFF;
    pl[22] = dst_ep;

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = cmd1_byte;
    req.payload = pl; req.payload_len = sizeof(pl);
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) {
        ESP_LOGE(TAG, "ZDO bind/unbind cmd=0x%02x: no SRSP", cmd1_byte);
        return false;
    }
    // SRSP status byte: 0x00 = success; others include INVALID_PARAMETER
    // (0xA0), NOT_SUPPORTED (0xAB), NO_ENTRY (0xB0). Before this check the
    // caller saw "ok" for any transport-level response, so bind failures
    // were silent — QWEN finding §1.
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "ZDO bind/unbind cmd=0x%02x status=0x%02x",
                 cmd1_byte, rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    ESP_LOGI(TAG, "ZDO bind/unbind cmd=0x%02x ok", cmd1_byte);
    return true;
}

bool zigbee_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    return zdo_bind_unbind(0x21, src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep);
}

bool zigbee_zdo_unbind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep) {
    return zdo_bind_unbind(0x22, src_nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep);
}

bool zigbee_leave_req(uint16_t nwk_addr, uint64_t ieee) {
    // ZDO_MGMT_LEAVE_REQ: [dst_addr 2B LE][ieee 8B LE][options=0x00 1B] = 11 bytes
    uint8_t pl[11];
    pl[0] = nwk_addr & 0xFF; pl[1] = (nwk_addr >> 8) & 0xFF;
    for (int i = 0; i < 8; i++) pl[2 + i] = (ieee >> (i * 8)) & 0xFF;
    pl[10] = 0x00;  // options: remove_children=0, rejoin=0

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x34;  // ZDO_MGMT_LEAVE_REQ
    req.payload = pl; req.payload_len = sizeof(pl);
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) {
        ESP_LOGE(TAG, "MGMT_LEAVE_REQ: no SRSP");
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "MGMT_LEAVE_REQ status=0x%02x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    ESP_LOGI(TAG, "leave_req nwk=0x%04x", nwk_addr);
    return true;
}

bool zigbee_zcl_level(uint16_t nwk_addr, uint8_t ep, uint8_t level,
                      uint16_t transition_tenths) {
    // ZCL MoveToLevelWithOnOff: cluster 0x0008, cmd 0x04
    // Payload: [level(1B), transition_time_low(1B), transition_time_high(1B)]
    uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[6] = {
        0x01,                              // frame_ctrl
        seq,                               // seq
        0x04,                              // cmd: MoveToLevelWithOnOff
        level,
        (uint8_t)(transition_tenths & 0xFF),
        (uint8_t)(transition_tenths >> 8),
    };

    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = nwk_addr & 0xFF;
    af_pl[1] = (nwk_addr >> 8) & 0xFF;
    af_pl[2] = ep;
    af_pl[3] = 0x01;        // src_ep
    af_pl[4] = 0x08; af_pl[5] = 0x00;  // cluster 0x0008 LE
    af_pl[6] = seq;         // trans_id
    af_pl[7] = 0x00;        // options
    af_pl[8] = 0x0F;        // radius
    af_pl[9] = sizeof(zcl_frame);
    for (size_t i = 0; i < sizeof(zcl_frame); i++)
        af_pl[10 + i] = zcl_frame[i];

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 3)) {
        ESP_LOGE(TAG, "ZCL Level: no SRSP");
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "ZCL Level: status=0x%02x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    ESP_LOGI(TAG, "ZCL Level level=%d trans=%d → nwk=0x%04x ep=%d",
             level, transition_tenths, nwk_addr, ep);
    return true;
}

bool zigbee_zcl_color_temp(uint16_t nwk_addr, uint8_t ep, uint16_t color_temp_mireds,
                            uint16_t transition_tenths) {
    // ZCL MoveToColorTemperature: cluster 0x0300, cmd 0x0A
    // Payload: [ct_low(1B), ct_high(1B), transition_low(1B), transition_high(1B)]
    uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[7] = {
        0x01,                                       // frame_ctrl
        seq,                                        // seq
        0x0A,                                       // cmd: MoveToColorTemperature
        (uint8_t)(color_temp_mireds & 0xFF),
        (uint8_t)(color_temp_mireds >> 8),
        (uint8_t)(transition_tenths & 0xFF),
        (uint8_t)(transition_tenths >> 8),
    };

    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = nwk_addr & 0xFF;
    af_pl[1] = (nwk_addr >> 8) & 0xFF;
    af_pl[2] = ep;
    af_pl[3] = 0x01;        // src_ep
    af_pl[4] = 0x00; af_pl[5] = 0x03;  // cluster 0x0300 LE
    af_pl[6] = seq;         // trans_id
    af_pl[7] = 0x00;        // options
    af_pl[8] = 0x0F;        // radius
    af_pl[9] = sizeof(zcl_frame);
    for (size_t i = 0; i < sizeof(zcl_frame); i++)
        af_pl[10 + i] = zcl_frame[i];

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 3)) {
        ESP_LOGE(TAG, "ZCL ColorTemp: no SRSP");
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "ZCL ColorTemp: status=0x%02x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    ESP_LOGI(TAG, "ZCL ColorTemp ct=%d trans=%d → nwk=0x%04x ep=%d",
             color_temp_mireds, transition_tenths, nwk_addr, ep);
    return true;
}

// ── zigbee_respond_gen_time ──────────────────────────────────────────
//
// Build a profile-wide Read Attributes Response for cluster 0x000A
// (genTime) and ship it in an AF_DATA_REQUEST. Tuya remotes spin on
// this read at join until they get a plausible LocalTime (attr 0x0007)
// back; only then do their firmwares arm button reporting. See
// `miboxerFut089zControls` in z2m's miboxer.ts for the same unlock
// flow.
//
// Request body layout (profile-wide, post-header):
//   attr_id_0 (2 LE), attr_id_1 (2 LE), …
// Response body layout (per attr):
//   attr_id (2 LE), status (1), [type (1), value (N)]   // type+value only on OK
//
// Zigbee LocalTime is UTC seconds since 2000-01-01 00:00:00. Linux
// epoch offset = 30 * 365.25 * 86400 = 946,684,800 seconds. If the
// system clock hasn't synchronised yet (time() returns a value below
// the epoch) we mark the attr UNSUPPORTED so the device moves on
// rather than receiving a bogus zero.
bool zigbee_respond_gen_time(uint16_t nwk_addr, uint8_t dst_ep,
                              uint8_t trans_seq,
                              const uint8_t* request_body,
                              size_t request_body_len) {
    constexpr uint32_t kEpoch2000 = 946684800u;  // s between 1970 and 2000

    const uint32_t now_unix = (uint32_t)time(nullptr);
    const bool     clock_ok = now_unix > kEpoch2000;
    const uint32_t utc_2000 = clock_ok ? (now_unix - kEpoch2000) : 0;

    // Build response body. Cap at 64 bytes — genTime has few attrs and
    // the Tuya remote only ever asks for one (0x0007).
    uint8_t body[80];
    body[0] = 0x18;          // profile-wide, direction=S→C, disable def-resp
    body[1] = trans_seq;     // mirror the request's TSN
    body[2] = 0x01;          // CMD_READ_ATTR_RESPONSE
    size_t body_len = 3;

    size_t i = 0;
    while (i + 1 < request_body_len && body_len + 8 <= sizeof(body)) {
        const uint16_t attr = static_cast<uint16_t>(request_body[i]) |
                              (static_cast<uint16_t>(request_body[i + 1]) << 8);
        i += 2;

        body[body_len++] = static_cast<uint8_t>(attr & 0xFF);
        body[body_len++] = static_cast<uint8_t>((attr >> 8) & 0xFF);

        if (attr == 0x0007 && clock_ok) {
            body[body_len++] = 0x00;   // SUCCESS
            body[body_len++] = 0xE2;   // ZclDataType UTC
            body[body_len++] = static_cast<uint8_t>(utc_2000 & 0xFF);
            body[body_len++] = static_cast<uint8_t>((utc_2000 >> 8) & 0xFF);
            body[body_len++] = static_cast<uint8_t>((utc_2000 >> 16) & 0xFF);
            body[body_len++] = static_cast<uint8_t>((utc_2000 >> 24) & 0xFF);
        } else {
            // UNSUPPORTED_ATTRIBUTE — stops the device probing further.
            body[body_len++] = 0x86;
        }
    }

    uint8_t af_pl[10 + sizeof(body)];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = dst_ep;          // remote endpoint that issued the read
    af_pl[3] = 0x01;            // coord EP1 sources the response
    af_pl[4] = 0x0A; af_pl[5] = 0x00;  // genTime cluster 0x000A LE
    af_pl[6] = trans_seq;       // AF trans id = ZCL TSN
    af_pl[7] = 0x00;            // options
    af_pl[8] = 0x0F;            // radius (15 hops)
    af_pl[9] = static_cast<uint8_t>(body_len);
    std::memcpy(af_pl + 10, body, body_len);

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;   // AF_DATA_REQUEST
    req.payload = af_pl; req.payload_len = static_cast<uint16_t>(10 + body_len);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "genTime response: no SRSP nwk=0x%04x", nwk_addr);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "genTime response: status=0x%02x nwk=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF, nwk_addr);
        return false;
    }
    ESP_LOGI(TAG, "genTime read-resp -> nwk=0x%04x ep=%u utc2000=%lu%s",
             nwk_addr, dst_ep,
             static_cast<unsigned long>(utc_2000),
             clock_ok ? "" : " (clock not synced, sent UNSUPPORTED)");
    return true;
}

// ── zigbee_send_default_response ─────────────────────────────────────
//
// Builds a ZCL Default Response (global cmd 0x0B) and ships it in an
// AF_DATA_REQUEST. Called from `zcl_attr_task` for every unicast frame
// whose frame-control "disable default response" bit is clear. Without
// this, Tuya sleepy devices (MiBoxer FUT089Z, several TS0601 remotes)
// retransmit identical Basic-cluster reports at APS level up to 5
// times, cluttering the log and wasting air-time after rejoin.
//
// ZCL spec notes encoded here:
//   * frame type = global (bits 1:0 = 00)
//   * direction  = opposite of incoming (bit 3 flipped)
//   * mfg-spec bit + mfg code copied from incoming
//   * "disable default response" set on the response itself (bit 4) so
//     the device doesn't ping-pong responses back at us
//
// Payload after the ZCL header is exactly two bytes: the incoming cmd
// id being ACK'd and a status byte (0x00 SUCCESS for any frame we've
// at least seen, even if we dispatched no converter).
bool zigbee_send_default_response(uint16_t nwk_addr, uint8_t dst_ep,
                                   uint16_t cluster_id,
                                   uint8_t incoming_fc, uint16_t mfg_code,
                                   uint8_t tsn, uint8_t cmd_id,
                                   uint8_t status) {
    const bool mfg = (incoming_fc & 0x04) != 0;

    uint8_t out_fc = 0x00;
    out_fc |= 0x10;                                // disable default response on reply
    if ((incoming_fc & 0x08) == 0) out_fc |= 0x08; // invert direction bit
    if (mfg)                       out_fc |= 0x04; // carry mfg-spec bit

    uint8_t body[8];
    std::size_t bl = 0;
    body[bl++] = out_fc;
    if (mfg) {
        body[bl++] = static_cast<uint8_t>(mfg_code & 0xFF);
        body[bl++] = static_cast<uint8_t>((mfg_code >> 8) & 0xFF);
    }
    body[bl++] = tsn;
    body[bl++] = 0x0B;   // ZCL Default Response
    body[bl++] = cmd_id; // original command
    body[bl++] = status;

    uint8_t af_pl[10 + sizeof(body)];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = dst_ep;
    af_pl[3] = 0x01;    // coord EP1
    af_pl[4] = static_cast<uint8_t>(cluster_id & 0xFF);
    af_pl[5] = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af_pl[6] = tsn;
    af_pl[7] = 0x00;    // options
    af_pl[8] = 0x0F;    // radius
    af_pl[9] = static_cast<uint8_t>(bl);
    std::memcpy(af_pl + 10, body, bl);

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;  // AF_DATA_REQUEST
    req.payload = af_pl;
    req.payload_len = static_cast<uint16_t>(10 + bl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 1000, 1)) {
        ESP_LOGD(TAG, "default-resp: no SRSP nwk=0x%04x cluster=0x%04x",
                 nwk_addr, cluster_id);
        return false;
    }
    return rsp.payload_len >= 1 && rsp.payload[0] == 0x00;
}

// ── zigbee_zcl_read / zigbee_zcl_cluster_command ────────────────────
//
// Single-shot AF_DATA_REQUEST wrappers that build a ZCL header +
// payload and hand it to ZNP. Used by `run_configure` when walking
// declarative `ConfigStep` pipelines so each device's magic-packet /
// custom-command sequence lives in .rodata, not hard-coded C++.

static constexpr std::size_t kAfHeaderLen = 10;
static constexpr std::size_t kAfPayloadMax = 80;   // ZCL body cap per call

bool zigbee_zcl_read(uint16_t nwk_addr, uint8_t endpoint,
                      uint16_t cluster_id,
                      const uint8_t* attr_ids_le, uint8_t attr_count,
                      uint16_t manu_code) {
    if (!attr_ids_le || attr_count == 0) return false;
    // ZCL header: profile-wide = 3 B (FC|TSN|CMD); manu-specific = 5 B
    // (FC|manu_lo|manu_hi|TSN|CMD).
    const std::size_t hdr_len = manu_code ? 5u : 3u;
    const std::size_t body_len = hdr_len + static_cast<std::size_t>(attr_count) * 2;
    if (body_len > kAfPayloadMax) return false;

    const uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[kAfPayloadMax];
    if (manu_code) {
        zcl_frame[0] = 0x04;   // FC: profile-wide, manu-specific, C→S
        zcl_frame[1] = static_cast<uint8_t>(manu_code & 0xFF);
        zcl_frame[2] = static_cast<uint8_t>((manu_code >> 8) & 0xFF);
        zcl_frame[3] = seq;
        zcl_frame[4] = 0x00;   // CMD_READ_ATTR
    } else {
        zcl_frame[0] = 0x00;   // FC: profile-wide, C→S
        zcl_frame[1] = seq;
        zcl_frame[2] = 0x00;   // CMD_READ_ATTR
    }
    std::memcpy(zcl_frame + hdr_len, attr_ids_le,
                static_cast<std::size_t>(attr_count) * 2);

    uint8_t af_pl[kAfHeaderLen + kAfPayloadMax];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = endpoint;
    af_pl[3] = 0x01;
    af_pl[4] = static_cast<uint8_t>(cluster_id & 0xFF);
    af_pl[5] = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af_pl[6] = seq;
    af_pl[7] = 0x00;   // options
    af_pl[8] = 0x0F;   // radius
    af_pl[9] = static_cast<uint8_t>(body_len);
    std::memcpy(af_pl + kAfHeaderLen, zcl_frame, body_len);

    MtFrame req{}, rsp{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl;
    req.payload_len = static_cast<uint16_t>(kAfHeaderLen + body_len);
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "zcl_read: no SRSP nwk=0x%04x cluster=0x%04x",
                 nwk_addr, cluster_id);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "zcl_read: status=0x%02x nwk=0x%04x cluster=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF,
                 nwk_addr, cluster_id);
        return false;
    }
    ESP_LOGI(TAG, "zcl_read -> nwk=0x%04x ep=%u cluster=0x%04x attrs=%u",
             nwk_addr, endpoint, cluster_id, attr_count);
    return true;
}

bool zigbee_zcl_write_attr(uint16_t nwk_addr, uint8_t endpoint,
                            uint16_t cluster_id, uint16_t attr_id,
                            uint8_t type, const uint8_t* value,
                            uint8_t value_len) {
    if (!value || value_len == 0) return false;
    // ZCL header (3) + record (attr_id 2 + type 1 + value N).
    const std::size_t body_len =
        3u + 3u + static_cast<std::size_t>(value_len);
    if (body_len > kAfPayloadMax) return false;

    const uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[kAfPayloadMax];
    zcl_frame[0] = 0x00;   // FC: profile-wide, C→S
    zcl_frame[1] = seq;
    zcl_frame[2] = 0x02;   // CMD_WRITE_ATTR
    zcl_frame[3] = static_cast<uint8_t>(attr_id & 0xFF);
    zcl_frame[4] = static_cast<uint8_t>((attr_id >> 8) & 0xFF);
    zcl_frame[5] = type;
    std::memcpy(zcl_frame + 6, value, value_len);

    uint8_t af_pl[kAfHeaderLen + kAfPayloadMax];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = endpoint;
    af_pl[3] = 0x01;
    af_pl[4] = static_cast<uint8_t>(cluster_id & 0xFF);
    af_pl[5] = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af_pl[6] = seq;
    af_pl[7] = 0x00;   // options
    af_pl[8] = 0x0F;   // radius
    af_pl[9] = static_cast<uint8_t>(body_len);
    std::memcpy(af_pl + kAfHeaderLen, zcl_frame, body_len);

    MtFrame req{}, rsp{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl;
    req.payload_len = static_cast<uint16_t>(kAfHeaderLen + body_len);
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "zcl_write: no SRSP nwk=0x%04x cluster=0x%04x attr=0x%04x",
                 nwk_addr, cluster_id, attr_id);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "zcl_write: status=0x%02x nwk=0x%04x cluster=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF,
                 nwk_addr, cluster_id);
        return false;
    }
    ESP_LOGI(TAG, "zcl_write -> nwk=0x%04x ep=%u cluster=0x%04x attr=0x%04x type=0x%02x",
             nwk_addr, endpoint, cluster_id, attr_id, type);
    return true;
}

// Shared core. When `confirm_timeout_ms > 0`, reserves a confirm ring
// slot BEFORE the SREQ is queued (so a racing AREQ isn't dropped) and
// blocks on it after SRSP success. Caller can opt out by passing 0.
static bool zcl_cluster_command_impl(uint16_t nwk_addr, uint8_t endpoint,
                                      uint16_t cluster_id, uint8_t cmd_id,
                                      const uint8_t* payload, uint8_t payload_len,
                                      uint8_t flags,
                                      uint32_t confirm_timeout_ms) {
    const std::size_t body_len = 3 + static_cast<std::size_t>(payload_len);
    if (body_len > kAfPayloadMax) return false;

    // FC byte: bit 0 = cluster-specific (1), bit 3 = direction C→S (0),
    // bit 4 = disable default response. Manu-specific (bit 2) isn't
    // wired today — callers that need a manu code must extend the API.
    uint8_t fc = 0x01;
    if (flags & 0x01) fc |= 0x10;   // kStepFlagDisableDefaultResponse

    const uint8_t seq = zcl_seq_next();

    // Reserve the confirm slot BEFORE posting the request, so a
    // fast AREQ lands on a waiting slot rather than falling off.
    const int confirm_slot =
        (confirm_timeout_ms > 0) ? znp_confirm_reserve(seq) : -1;

    uint8_t zcl_frame[kAfPayloadMax];
    zcl_frame[0] = fc;
    zcl_frame[1] = seq;
    zcl_frame[2] = cmd_id;
    if (payload && payload_len)
        std::memcpy(zcl_frame + 3, payload, payload_len);

    uint8_t af_pl[kAfHeaderLen + kAfPayloadMax];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = endpoint;
    af_pl[3] = 0x01;
    af_pl[4] = static_cast<uint8_t>(cluster_id & 0xFF);
    af_pl[5] = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af_pl[6] = seq;
    af_pl[7] = 0x00;
    af_pl[8] = 0x0F;
    af_pl[9] = static_cast<uint8_t>(body_len);
    std::memcpy(af_pl + kAfHeaderLen, zcl_frame, body_len);

    MtFrame req{}, rsp{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl;
    req.payload_len = static_cast<uint16_t>(kAfHeaderLen + body_len);
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "zcl_cmd: no SRSP nwk=0x%04x cluster=0x%04x cmd=0x%02x",
                 nwk_addr, cluster_id, cmd_id);
        if (confirm_slot >= 0) znp_confirm_release(confirm_slot);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "zcl_cmd: status=0x%02x nwk=0x%04x cluster=0x%04x cmd=0x%02x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF,
                 nwk_addr, cluster_id, cmd_id);
        if (confirm_slot >= 0) znp_confirm_release(confirm_slot);
        return false;
    }

    if (confirm_slot >= 0) {
        const int mac_status = znp_confirm_wait(confirm_slot, confirm_timeout_ms);
        if (mac_status != 0) {
            ESP_LOGW(TAG,
                     "zcl_cmd MAC %s nwk=0x%04x cluster=0x%04x cmd=0x%02x "
                     "trans=0x%02x%s%x",
                     mac_status < 0 ? "timeout" : "fail",
                     nwk_addr, cluster_id, cmd_id, seq,
                     mac_status < 0 ? "" : " status=0x",
                     mac_status < 0 ? 0 : static_cast<unsigned>(mac_status));
            return false;
        }
    }

    ESP_LOGI(TAG, "zcl_cmd -> nwk=0x%04x ep=%u cluster=0x%04x cmd=0x%02x len=%u flags=0x%02x%s",
             nwk_addr, endpoint, cluster_id, cmd_id, payload_len, flags,
             confirm_slot >= 0 ? " (MAC-confirmed)" : "");
    return true;
}

bool zigbee_zcl_cluster_command(uint16_t nwk_addr, uint8_t endpoint,
                                 uint16_t cluster_id, uint8_t cmd_id,
                                 const uint8_t* payload, uint8_t payload_len,
                                 uint8_t flags) {
    return zcl_cluster_command_impl(nwk_addr, endpoint, cluster_id, cmd_id,
                                     payload, payload_len, flags, 0);
}

bool zigbee_zcl_cluster_command_wait_confirm(uint16_t nwk_addr, uint8_t endpoint,
                                              uint16_t cluster_id, uint8_t cmd_id,
                                              const uint8_t* payload,
                                              uint8_t payload_len,
                                              uint8_t flags,
                                              uint32_t confirm_timeout_ms) {
    if (confirm_timeout_ms == 0) confirm_timeout_ms = 2000;
    return zcl_cluster_command_impl(nwk_addr, endpoint, cluster_id, cmd_id,
                                     payload, payload_len, flags,
                                     confirm_timeout_ms);
}

// ── zigbee_tuya_magic_packet ─────────────────────────────────────────
//
// z2m-source: `lib/tuya.ts` `configureMagicPacket`. Reads six genBasic
// attrs from the device; the act of answering (or even attempting to
// answer) the probe unlocks Tuya firmware that gates command reporting
// on this handshake.
bool zigbee_tuya_magic_packet(uint16_t nwk_addr, uint8_t dst_ep) {
    const uint8_t seq = zcl_seq_next();
    const uint8_t zcl_frame[] = {
        0x00,                 // FC: profile-wide, C→S, default-resp enabled
        seq,                  // TSN
        0x00,                 // CMD_READ_ATTR
        0x04, 0x00,           // attr 0x0004 manufacturerName
        0x00, 0x00,           // attr 0x0000 zclVersion
        0x01, 0x00,           // attr 0x0001 applicationVersion
        0x05, 0x00,           // attr 0x0005 modelIdentifier
        0x07, 0x00,           // attr 0x0007 powerSource
        0xFE, 0xFF,           // attr 0xFFFE attributeReportingStatus
    };

    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = dst_ep;           // device EP (usually 1)
    af_pl[3] = 0x01;             // coord src EP
    af_pl[4] = 0x00; af_pl[5] = 0x00;  // genBasic cluster 0x0000
    af_pl[6] = seq;
    af_pl[7] = 0x00;             // options
    af_pl[8] = 0x0F;             // radius
    af_pl[9] = sizeof(zcl_frame);
    std::memcpy(af_pl + 10, zcl_frame, sizeof(zcl_frame));

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;   // AF_DATA_REQUEST
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "tuya magic packet: no SRSP nwk=0x%04x", nwk_addr);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "tuya magic packet: status=0x%02x nwk=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF, nwk_addr);
        return false;
    }
    ESP_LOGI(TAG, "tuya magic packet -> nwk=0x%04x ep=%u", nwk_addr, dst_ep);
    return true;
}

// ── zigbee_miboxer_fut089z_finalize ──────────────────────────────────
//
// z2m-source: `src/devices/miboxer.ts` (FUT089Z). Fires the two Tuya
// custom commands that actually arm the remote's button reporting:
//   1. genGroups (0x0004) cmd 0xF0 miboxerSetZones
//      payload: u8 count=8, then 8× (u16 groupId LE, u8 zoneNum)
//      default mapping: zoneNum 1..8 → groupId 101..108.
//   2. genBasic (0x0000) cmd 0xF0 tuyaSetup
//      payload: empty, FC bit 4 = disable default response.
//
// Both use AF_DATA_REQUEST. We don't parse responses — the remote
// flips its internal arm flag as soon as it services the commands.
static bool miboxer_set_zones(uint16_t nwk, uint8_t dst_ep) {
    const uint8_t seq = zcl_seq_next();

    // ZCL frame: FC 0x01 (cluster-specific, C→S, default resp on),
    // TSN, CmdId 0xF0, then count + 8 zone tuples (1 + 8*3 = 25 B).
    uint8_t zcl_frame[3 + 1 + 8 * 3];
    zcl_frame[0] = 0x01;
    zcl_frame[1] = seq;
    zcl_frame[2] = 0xF0;
    zcl_frame[3] = 8;   // zone count

    for (uint8_t z = 1; z <= 8; ++z) {
        const uint16_t group_id = static_cast<uint16_t>(100 + z);
        const size_t   off      = 4 + (z - 1) * 3;
        zcl_frame[off + 0] = static_cast<uint8_t>(group_id & 0xFF);
        zcl_frame[off + 1] = static_cast<uint8_t>((group_id >> 8) & 0xFF);
        zcl_frame[off + 2] = z;   // zoneNum
    }

    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = static_cast<uint8_t>(nwk & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk >> 8) & 0xFF);
    af_pl[2] = dst_ep;
    af_pl[3] = 0x01;
    af_pl[4] = 0x04; af_pl[5] = 0x00;   // genGroups cluster 0x0004
    af_pl[6] = seq;
    af_pl[7] = 0x00;
    af_pl[8] = 0x0F;
    af_pl[9] = sizeof(zcl_frame);
    std::memcpy(af_pl + 10, zcl_frame, sizeof(zcl_frame));

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "miboxerSetZones: no SRSP nwk=0x%04x", nwk);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "miboxerSetZones: status=0x%02x nwk=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF, nwk);
        return false;
    }
    ESP_LOGI(TAG, "miboxerSetZones -> nwk=0x%04x ep=%u (8 zones)", nwk, dst_ep);
    return true;
}

static bool miboxer_tuya_setup(uint16_t nwk, uint8_t dst_ep) {
    const uint8_t seq = zcl_seq_next();

    // FC 0x11 = cluster-specific + disable default response.
    uint8_t zcl_frame[3] = { 0x11, seq, 0xF0 };

    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = static_cast<uint8_t>(nwk & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk >> 8) & 0xFF);
    af_pl[2] = dst_ep;
    af_pl[3] = 0x01;
    af_pl[4] = 0x00; af_pl[5] = 0x00;   // genBasic cluster 0x0000
    af_pl[6] = seq;
    af_pl[7] = 0x00;
    af_pl[8] = 0x0F;
    af_pl[9] = sizeof(zcl_frame);
    std::memcpy(af_pl + 10, zcl_frame, sizeof(zcl_frame));

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;
    req.payload = af_pl; req.payload_len = sizeof(af_pl);

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "tuyaSetup: no SRSP nwk=0x%04x", nwk);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGW(TAG, "tuyaSetup: status=0x%02x nwk=0x%04x",
                 rsp.payload_len ? rsp.payload[0] : 0xFF, nwk);
        return false;
    }
    ESP_LOGI(TAG, "tuyaSetup -> nwk=0x%04x ep=%u", nwk, dst_ep);
    return true;
}

bool zigbee_miboxer_fut089z_finalize(uint16_t nwk_addr, uint8_t dst_ep) {
    const bool z = miboxer_set_zones(nwk_addr, dst_ep);
    const bool s = miboxer_tuya_setup(nwk_addr, dst_ep);
    return z && s;
}

bool zigbee_pool_remove(uint64_t ieee) {
    for (uint16_t i = 0; i < s_pool_count; i++) {
        if (s_pool[i].ieee_addr == ieee) {
            pool_remove(i);
            // Drop the adapter's cached def pointer so if a new device
            // ever claims this ieee again we don't serve the old port.
            zhac_adapter_invalidate_def_cache(ieee);
            ESP_LOGI(TAG, "pool_remove ieee=0x%016llX", (unsigned long long)ieee);
            return true;
        }
    }
    return false;
}
