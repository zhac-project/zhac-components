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

// ── af_data_request ──────────────────────────────────────────────────────
// §4 (FINDINGS.md, zcl_commands.cpp:27 DUP + :45 SMELL→correctness).
//
// The AF_DATA_REQUEST header assembly + SRSP status check was copy-pasted
// across ~10 call-sites here (on/off, level, color-temp, gen-time,
// default-resp, read, write, configure-report, cluster-command, magic
// packet, miboxer) and in zhc_send_bridge.cpp, each with subtly different
// retry behaviour (timeouts 1000/2000/3000, attempts 1/2/3). This one
// helper folds the duplication while PRESERVING each call-site's timeout
// and attempt count via parameters — behaviour is not homogenised.
//
// It also fixes the on-air double-fire (:45): AF_DATA_REQUEST was blindly
// retried up to 3× with the SAME trans_id after an SRSP loss. The SRSP only
// confirms the NCP *accepted* the frame; if attempt 1 actually reached the
// NCP but the SRSP was dropped on the UART, a blind re-send executes the
// command twice on-air (a Toggle visibly double-fires, a level/color step
// jumps twice). For non-idempotent commands we therefore do NOT blind-retry
// the SREQ: we send once and gate success on the asynchronous
// AF_DATA_CONFIRM (MAC delivery) via the existing znp_confirm ring — the
// same mechanism zcl_cluster_command_impl already uses. Idempotent frames
// (attribute reads, configure-reporting, the gen-time / Basic probes) keep
// their multi-attempt SREQ retry since re-sending them is harmless.
//
// `body`/`body_len` is the fully-formed ZCL frame (FC|TSN|CMD|…). `trans_id`
// is written into the AF header trans_id field and MUST equal the ZCL TSN so
// the confirm correlates. Returns true when the NCP accepted the frame (and,
// for non-idempotent commands, the MAC confirm reported success or the
// confirm ring was saturated — see below).
struct AfReqOpts {
    uint32_t srsp_timeout_ms;   // per-attempt SRSP wait
    int      srsp_attempts;     // SREQ attempts (forced to 1 when !idempotent)
    bool     idempotent;        // true = safe to blind-retry the SREQ
    uint32_t confirm_timeout_ms;// AF_DATA_CONFIRM wait for non-idempotent (0=skip)
    esp_log_level_t fail_level; // ESP_LOG_ERROR / WARN / DEBUG for the no-SRSP log
    const char* what;           // short label for logs
};

static bool af_data_request(uint16_t nwk_addr, uint8_t dst_ep, uint8_t src_ep,
                            uint16_t cluster_id, uint8_t trans_id,
                            const uint8_t* body, size_t body_len,
                            const AfReqOpts& o) {
    // AF_DATA_REQUEST payload (Z-Stack 3.x):
    //   [dst_addr 2B LE, dst_ep, src_ep, cluster 2B LE,
    //    trans_id, options, radius, len, data...]
    if (body_len > 0xFF) return false;
    uint8_t af_pl[10 + 250];
    if (body_len > sizeof(af_pl) - 10) return false;
    af_pl[0] = static_cast<uint8_t>(nwk_addr & 0xFF);
    af_pl[1] = static_cast<uint8_t>((nwk_addr >> 8) & 0xFF);
    af_pl[2] = dst_ep;
    af_pl[3] = src_ep;
    af_pl[4] = static_cast<uint8_t>(cluster_id & 0xFF);
    af_pl[5] = static_cast<uint8_t>((cluster_id >> 8) & 0xFF);
    af_pl[6] = trans_id;
    af_pl[7] = 0x00;        // options
    af_pl[8] = 0x0F;        // radius (15 hops)
    af_pl[9] = static_cast<uint8_t>(body_len);
    if (body && body_len) std::memcpy(af_pl + 10, body, body_len);

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;   // AF_DATA_REQUEST
    req.payload = af_pl;
    req.payload_len = static_cast<uint16_t>(10 + body_len);

    // Non-idempotent: reserve the confirm slot BEFORE the SREQ so a fast
    // AF_DATA_CONFIRM isn't dropped, send exactly once, then gate on the
    // confirm. Idempotent: keep the (harmless) multi-attempt SREQ retry.
    const bool gate_confirm = !o.idempotent && o.confirm_timeout_ms > 0;
    const int  confirm_slot = gate_confirm ? znp_confirm_reserve(trans_id) : -1;
    const int  attempts     = o.idempotent ? o.srsp_attempts : 1;

    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, o.srsp_timeout_ms, attempts)) {
        ESP_LOG_LEVEL(o.fail_level, TAG, "%s: no SRSP nwk=0x%04x cluster=0x%04x",
                      o.what, nwk_addr, cluster_id);
        if (confirm_slot >= 0) znp_confirm_release(confirm_slot);
        return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOG_LEVEL(o.fail_level, TAG, "%s: status=0x%02x nwk=0x%04x cluster=0x%04x",
                      o.what, rsp.payload_len ? rsp.payload[0] : 0xFF,
                      nwk_addr, cluster_id);
        if (confirm_slot >= 0) znp_confirm_release(confirm_slot);
        return false;
    }

    if (confirm_slot >= 0) {
        const int mac = znp_confirm_wait(confirm_slot, o.confirm_timeout_ms);
        if (mac != 0) {
            // MAC delivery failed/timed out. We deliberately do NOT re-send
            // (that is exactly the double-fire we are avoiding) — report the
            // failure and let the higher layer decide. A timeout here just
            // means we never saw the confirm; the frame may still have been
            // delivered, so a blind re-send would risk a duplicate.
            ESP_LOGW(TAG, "%s: MAC %s nwk=0x%04x cluster=0x%04x trans=0x%02x",
                     o.what, mac < 0 ? "timeout" : "fail",
                     nwk_addr, cluster_id, trans_id);
            return false;
        }
    } else if (gate_confirm) {
        // Ring saturated (reserve returned -1). The SREQ succeeded and we
        // could not arm a confirm waiter; accept the SRSP as our best
        // signal rather than blind-retrying. Logged inside znp_confirm.
    }
    return true;
}

// Exported thin wrapper (declared in zigbee_mgr.h) so other .cpp in this
// component — notably zhc_send_bridge.cpp, the generic adapter→radio path —
// route through the SAME af_data_request builder + no-blind-retry policy.
bool zigbee_af_send_zcl(uint16_t nwk_addr, uint8_t dst_ep,
                        uint16_t cluster_id, uint8_t trans_id,
                        const uint8_t* body, uint32_t body_len,
                        bool idempotent, uint32_t confirm_timeout_ms) {
    const AfReqOpts opts{2000, idempotent ? 2 : 1, idempotent,
                         confirm_timeout_ms, ESP_LOG_ERROR, "zhc send"};
    return af_data_request(nwk_addr, dst_ep, 0x01, cluster_id, trans_id,
                           body, body_len, opts);
}

bool zigbee_zcl_on_off(uint16_t nwk_addr, uint8_t ep, uint8_t cmd) {
    // ZCL frame: [frame_ctrl=0x01, seq, cmd_id=cmd]. Seq is the global ZCL
    // counter (zcl_seq_next) shared with the converter layer so responses
    // correlate correctly — a hardcoded 0x01 made every command look like
    // the same transaction to some devices (CODEX §6).
    const uint8_t seq = zcl_seq_next();
    const uint8_t zcl_frame[3] = {0x01, seq, cmd};
    // NON-IDEMPOTENT: On/Off (esp. Toggle 0x02) MUST NOT blind-retry the
    // SREQ — a dropped SRSP after the NCP already accepted attempt 1 would
    // toggle the load twice. Single send, gate on AF_DATA_CONFIRM (§4 :45).
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL On/Off"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0006, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "ZCL On/Off cmd=0x%02x → nwk=0x%04x ep=%d", cmd, nwk_addr, ep);
    return true;
}

// ZCL Groups cluster (0x0004): add/remove a device endpoint's group membership.
// Groups is a DEVICE-SIDE table — a light that is a member of group N obeys any
// command sent to group N, including the groupcasts a hardware zone-remote
// emits (MiBoxer FUT089Z: zones 1-8 = groups 101-108). This is native ZCL
// membership, distinct from ZHAC's synthetic (gateway fan-out) groups. We gate
// on the APS delivery confirm only; the device's ZCL Group (Add/Remove) Response
// status is not parsed here — authoritative state comes from Get Group
// Membership (the "refresh from device" path).
bool zigbee_zcl_group_add(uint16_t nwk_addr, uint8_t ep, uint16_t group_id) {
    const uint8_t seq = zcl_seq_next();
    // FC 0x01 (cluster-specific, C→S), seq, cmd 0x00 (Add Group),
    // payload = group_id (2 B LE) + name length 0x00 (no name).
    const uint8_t zcl_frame[6] = {
        0x01, seq, 0x00,
        static_cast<uint8_t>(group_id & 0xFF),
        static_cast<uint8_t>((group_id >> 8) & 0xFF),
        0x00,
    };
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL Group Add"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0004, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "ZCL Group Add gid=%u → nwk=0x%04x ep=%d", group_id, nwk_addr, ep);
    return true;
}

bool zigbee_zcl_group_remove(uint16_t nwk_addr, uint8_t ep, uint16_t group_id) {
    const uint8_t seq = zcl_seq_next();
    // FC 0x01, seq, cmd 0x03 (Remove Group), payload = group_id (2 B LE).
    const uint8_t zcl_frame[5] = {
        0x01, seq, 0x03,
        static_cast<uint8_t>(group_id & 0xFF),
        static_cast<uint8_t>((group_id >> 8) & 0xFF),
    };
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL Group Remove"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0004, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "ZCL Group Remove gid=%u → nwk=0x%04x ep=%d", group_id, nwk_addr, ep);
    return true;
}

bool zigbee_zcl_identify(uint16_t nwk_addr, uint8_t ep, uint16_t seconds) {
    const uint8_t seq = zcl_seq_next();
    // ZCL Identify (0x0003) cmd 0x00 (Identify): FC 0x01, seq, cmd, then the
    // IdentifyTime attribute value (u16 LE, seconds the device blinks/beeps).
    const uint8_t zcl_frame[5] = {
        0x01, seq, 0x00,
        static_cast<uint8_t>(seconds & 0xFF),
        static_cast<uint8_t>((seconds >> 8) & 0xFF),
    };
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL Identify"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0003, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "ZCL Identify %us → nwk=0x%04x ep=%d", seconds, nwk_addr, ep);
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
    const uint8_t seq = zcl_seq_next();
    const uint8_t zcl_frame[6] = {
        0x01,                              // frame_ctrl
        seq,                               // seq
        0x04,                              // cmd: MoveToLevelWithOnOff
        level,
        (uint8_t)(transition_tenths & 0xFF),
        (uint8_t)(transition_tenths >> 8),
    };
    // NON-IDEMPOTENT: a re-sent level step moves twice (visible flicker /
    // overshoot). Single send + AF_DATA_CONFIRM gate (§4 :45).
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL Level"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0008, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "ZCL Level level=%d trans=%d → nwk=0x%04x ep=%d",
             level, transition_tenths, nwk_addr, ep);
    return true;
}

bool zigbee_zcl_color_temp(uint16_t nwk_addr, uint8_t ep, uint16_t color_temp_mireds,
                            uint16_t transition_tenths) {
    // ZCL MoveToColorTemperature: cluster 0x0300, cmd 0x0A
    // Payload: [ct_low(1B), ct_high(1B), transition_low(1B), transition_high(1B)]
    const uint8_t seq = zcl_seq_next();
    const uint8_t zcl_frame[7] = {
        0x01,                                       // frame_ctrl
        seq,                                        // seq
        0x0A,                                       // cmd: MoveToColorTemperature
        (uint8_t)(color_temp_mireds & 0xFF),
        (uint8_t)(color_temp_mireds >> 8),
        (uint8_t)(transition_tenths & 0xFF),
        (uint8_t)(transition_tenths >> 8),
    };
    // NON-IDEMPOTENT: re-sent color step overshoots. Single send + confirm.
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_ERROR, "ZCL ColorTemp"};
    if (!af_data_request(nwk_addr, ep, 0x01, 0x0300, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
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

    // IDEMPOTENT (a read RESPONSE): re-sending the same time read-response
    // is harmless, so keep the multi-attempt SREQ retry. WARN on failure.
    const AfReqOpts opts{2000, 2, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_WARN, "genTime response"};
    if (!af_data_request(nwk_addr, dst_ep, 0x01, 0x000A, trans_seq,
                         body, body_len, opts))
        return false;
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

    // IDEMPOTENT (a Default Response ACK, with disable-default-response set
    // so it never ping-pongs): re-sending is harmless. Preserve the
    // original best-effort profile — 1 attempt, 1000 ms, DEBUG on miss.
    const AfReqOpts opts{1000, 1, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_DEBUG, "default-resp"};
    return af_data_request(nwk_addr, dst_ep, 0x01, cluster_id, tsn,
                           body, bl, opts);
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

    // IDEMPOTENT (Read Attributes — no device-side state change): keep the
    // multi-attempt SREQ retry, WARN on failure.
    const AfReqOpts opts{2000, 2, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_WARN, "zcl_read"};
    if (!af_data_request(nwk_addr, endpoint, 0x01, cluster_id, seq,
                         zcl_frame, body_len, opts))
        return false;
    ESP_LOGI(TAG, "zcl_read -> nwk=0x%04x ep=%u cluster=0x%04x attrs=%u",
             nwk_addr, endpoint, cluster_id, attr_count);
    return true;
}

bool zigbee_zcl_write_attr(uint16_t nwk_addr, uint8_t endpoint,
                            uint16_t cluster_id, uint16_t attr_id,
                            uint8_t type, const uint8_t* value,
                            uint8_t value_len,
                            uint16_t manu_code) {
    if (!value || value_len == 0) return false;
    // ZCL header: profile-wide = 3 B (FC|TSN|CMD); manu-specific = 5 B
    // (FC|manu_lo|manu_hi|TSN|CMD) — mirrors zigbee_zcl_read above.
    // Record = attr_id 2 + type 1 + value N.
    const std::size_t hdr_len = manu_code ? 5u : 3u;
    const std::size_t rec_len = 3u + static_cast<std::size_t>(value_len);
    const std::size_t body_len = hdr_len + rec_len;
    if (body_len > kAfPayloadMax) return false;

    const uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[kAfPayloadMax];
    std::size_t pos = 0;
    if (manu_code) {
        zcl_frame[pos++] = 0x04;   // FC: profile-wide, manu-specific, C→S
        zcl_frame[pos++] = static_cast<uint8_t>(manu_code & 0xFF);
        zcl_frame[pos++] = static_cast<uint8_t>((manu_code >> 8) & 0xFF);
        zcl_frame[pos++] = seq;
        zcl_frame[pos++] = 0x02;   // CMD_WRITE_ATTR
    } else {
        zcl_frame[pos++] = 0x00;   // FC: profile-wide, C→S
        zcl_frame[pos++] = seq;
        zcl_frame[pos++] = 0x02;   // CMD_WRITE_ATTR
    }
    zcl_frame[pos++] = static_cast<uint8_t>(attr_id & 0xFF);
    zcl_frame[pos++] = static_cast<uint8_t>((attr_id >> 8) & 0xFF);
    zcl_frame[pos++] = type;
    std::memcpy(zcl_frame + pos, value, value_len);

    // IDEMPOTENT: Write Attributes sets an ABSOLUTE value (unlike a
    // relative On/Off Toggle or a Level/Color *move*), so writing the same
    // record twice lands on the same device state — a blind SREQ re-send is
    // safe. Keep the multi-attempt retry (matches the "Idempotent — safe to
    // write on every request" contract the IAS CIE-address writer relies on
    // in zigbee_mgr.cpp). WARN on failure.
    const AfReqOpts opts{2000, 2, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_WARN, "zcl_write"};
    if (!af_data_request(nwk_addr, endpoint, 0x01, cluster_id, seq,
                         zcl_frame, body_len, opts))
        return false;
    ESP_LOGI(TAG, "zcl_write -> nwk=0x%04x ep=%u cluster=0x%04x attr=0x%04x type=0x%02x",
             nwk_addr, endpoint, cluster_id, attr_id, type);
    return true;
}

// ── zigbee_zcl_configure_report ─────────────────────────────────────
//
// ZCL Configure Reporting (cmd 0x06) for ONE attribute.
//
// Wire format per ZCL §2.5.7:
//   FC | TSN | CMD=0x06 | [manu_code if manu_code != 0]
//   record: direction(1=0x00 send reports TO us)
//           attr_id(2 LE)
//           attr_type(1)
//           min_interval(2 LE seconds)
//           max_interval(2 LE seconds)
//           reportable_change(N — ANALOG types only; OMITTED for discrete)
//
// Reportable-change field width follows the attribute data type:
//   analog  (u8/s8 0x20/0x28, u16/s16 0x21/0x29, u32/s32 0x23/0x2B,
//            float32 0x38/0x39, …)  → 1/2/4 bytes matching the type
//   discrete (Bool 0x10, ENUM8 0x30, ENUM16 0x31, bitmap 0x18..0x1F)
//                                      → no change field at all
//
// SRSP success means ZNP queued the AF_DATA_REQUEST; the device's
// Configure Reporting Response with per-attribute status codes lands
// later as AF_INCOMING_MSG and traverses the normal decode pipeline.
// Matches z2m's `reporting.*` helpers — they don't await the response.
bool zigbee_zcl_configure_report(uint16_t nwk_addr, uint8_t endpoint,
                                  uint16_t cluster_id, uint16_t attr_id,
                                  uint8_t  attr_type,
                                  uint16_t min_interval,
                                  uint16_t max_interval,
                                  uint32_t reportable_change,
                                  uint16_t manu_code) {
    // Type → reportable-change byte width.
    // 0     = discrete type, no change field follows
    // 1/2/3/4 = analog field width
    // 0xFF  = unsupported type (caller-visible failure rather than
    //         silently truncating)
    auto change_width = [](uint8_t t) -> uint8_t {
        switch (t) {
            // Discrete types — no reportable_change.
            case 0x10:                              // Bool
            case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1C: case 0x1D: case 0x1E: case 0x1F:  // bitmap8..bitmap64
            case 0x30: case 0x31:                   // ENUM8 / ENUM16
                return 0;
            // 1-byte analog.
            case 0x20: case 0x28:                   // u8, s8
                return 1;
            // 2-byte analog.
            case 0x21: case 0x29: case 0x38:        // u16, s16, semi-float
                return 2;
            // 3-byte analog (uncommon).
            case 0x22: case 0x2A:                   // u24, s24
                return 3;
            // 4-byte analog.
            case 0x23: case 0x2B: case 0x39:        // u32, s32, float32
                return 4;
            default:
                return 0xFF;
        }
    };
    const uint8_t chg_w = change_width(attr_type);
    if (chg_w == 0xFF) return false;

    const std::size_t hdr_len = manu_code ? 5u : 3u;
    const std::size_t rec_len = 1u /*direction*/ + 2u /*attr_id*/ +
                                 1u /*type*/ + 2u /*min*/ + 2u /*max*/ +
                                 chg_w;
    const std::size_t body_len = hdr_len + rec_len;
    if (body_len > kAfPayloadMax) return false;

    const uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[kAfPayloadMax];
    std::size_t pos = 0;
    if (manu_code) {
        zcl_frame[pos++] = 0x04;   // FC: profile-wide, manu-specific, C→S
        zcl_frame[pos++] = static_cast<uint8_t>(manu_code & 0xFF);
        zcl_frame[pos++] = static_cast<uint8_t>((manu_code >> 8) & 0xFF);
        zcl_frame[pos++] = seq;
        zcl_frame[pos++] = 0x06;   // CMD_CONFIGURE_REPORTING
    } else {
        zcl_frame[pos++] = 0x00;   // FC: profile-wide, C→S
        zcl_frame[pos++] = seq;
        zcl_frame[pos++] = 0x06;
    }
    zcl_frame[pos++] = 0x00;       // direction: 0 = device reports TO us
    zcl_frame[pos++] = static_cast<uint8_t>(attr_id & 0xFF);
    zcl_frame[pos++] = static_cast<uint8_t>((attr_id >> 8) & 0xFF);
    zcl_frame[pos++] = attr_type;
    zcl_frame[pos++] = static_cast<uint8_t>(min_interval & 0xFF);
    zcl_frame[pos++] = static_cast<uint8_t>((min_interval >> 8) & 0xFF);
    zcl_frame[pos++] = static_cast<uint8_t>(max_interval & 0xFF);
    zcl_frame[pos++] = static_cast<uint8_t>((max_interval >> 8) & 0xFF);
    for (uint8_t i = 0; i < chg_w; ++i) {
        zcl_frame[pos++] = static_cast<uint8_t>(
            (reportable_change >> (8 * i)) & 0xFF);
    }

    // IDEMPOTENT (Configure Reporting sets absolute min/max/change — same
    // record twice = same config): keep multi-attempt SREQ retry, WARN.
    const AfReqOpts opts{2000, 2, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_WARN, "configure_report"};
    if (!af_data_request(nwk_addr, endpoint, 0x01, cluster_id, seq,
                         zcl_frame, body_len, opts))
        return false;
    ESP_LOGI(TAG,
              "configure_report -> nwk=0x%04x ep=%u cluster=0x%04x "
              "attr=0x%04x type=0x%02x min=%us max=%us chg=%lu",
              nwk_addr, endpoint, cluster_id, attr_id, attr_type,
              min_interval, max_interval,
              static_cast<unsigned long>(reportable_change));
    return true;
}

// Shared core for arbitrary cluster-specific commands. These are device
// commands (cover up/down/step, IR send, Tuya custom ops) and are treated
// as NON-IDEMPOTENT: we never blind-retry the SREQ (§4 :45 — a re-send
// after a lost SRSP could fire the command twice on-air). When
// `confirm_timeout_ms > 0` the helper reserves a confirm ring slot BEFORE
// the SREQ (so a racing AF_DATA_CONFIRM isn't dropped) and gates success on
// it; passing 0 is a best-effort single send (still no blind retry).
static bool zcl_cluster_command_impl(uint16_t nwk_addr, uint8_t endpoint,
                                      uint16_t cluster_id, uint8_t cmd_id,
                                      const uint8_t* payload, uint8_t payload_len,
                                      uint8_t flags,
                                      uint32_t confirm_timeout_ms) {
    if (payload_len > kAfPayloadMax - 3) return false;

    // FC byte: bit 0 = cluster-specific (1), bit 3 = direction C→S (0),
    // bit 4 = disable default response. Manu-specific (bit 2) isn't
    // wired today — callers that need a manu code must extend the API.
    uint8_t fc = 0x01;
    if (flags & 0x01) fc |= 0x10;   // kStepFlagDisableDefaultResponse

    const uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[kAfPayloadMax];
    zcl_frame[0] = fc;
    zcl_frame[1] = seq;
    zcl_frame[2] = cmd_id;
    if (payload && payload_len)
        std::memcpy(zcl_frame + 3, payload, payload_len);
    const std::size_t body_len = 3u + static_cast<std::size_t>(payload_len);

    const AfReqOpts opts{2000, 1, /*idempotent=*/false,
                         /*confirm=*/confirm_timeout_ms, ESP_LOG_WARN, "zcl_cmd"};
    if (!af_data_request(nwk_addr, endpoint, 0x01, cluster_id, seq,
                         zcl_frame, body_len, opts))
        return false;

    ESP_LOGI(TAG, "zcl_cmd -> nwk=0x%04x ep=%u cluster=0x%04x cmd=0x%02x len=%u flags=0x%02x%s",
             nwk_addr, endpoint, cluster_id, cmd_id, payload_len, flags,
             confirm_timeout_ms > 0 ? " (MAC-confirmed)" : "");
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

    // IDEMPOTENT (a genBasic Read probe): keep the multi-attempt retry.
    const AfReqOpts opts{2000, 2, /*idempotent=*/true, /*confirm=*/0,
                         ESP_LOG_WARN, "tuya magic packet"};
    if (!af_data_request(nwk_addr, dst_ep, 0x01, 0x0000, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
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

    // NON-IDEMPOTENT (cluster-specific 0xF0 device command): single send +
    // confirm gate, no blind SREQ retry (§4 :45).
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_WARN, "miboxerSetZones"};
    if (!af_data_request(nwk, dst_ep, 0x01, 0x0004, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "miboxerSetZones -> nwk=0x%04x ep=%u (8 zones)", nwk, dst_ep);
    return true;
}

static bool miboxer_tuya_setup(uint16_t nwk, uint8_t dst_ep) {
    const uint8_t seq = zcl_seq_next();

    // FC 0x11 = cluster-specific + disable default response.
    uint8_t zcl_frame[3] = { 0x11, seq, 0xF0 };

    // NON-IDEMPOTENT (cluster-specific 0xF0 device command; FC already has
    // disable-default-response set in zcl_frame[0]). Single send + confirm.
    const AfReqOpts opts{2000, 1, /*idempotent=*/false, /*confirm=*/2000,
                         ESP_LOG_WARN, "tuyaSetup"};
    if (!af_data_request(nwk, dst_ep, 0x01, 0x0000, seq,
                         zcl_frame, sizeof(zcl_frame), opts))
        return false;
    ESP_LOGI(TAG, "tuyaSetup -> nwk=0x%04x ep=%u", nwk, dst_ep);
    return true;
}

bool zigbee_miboxer_fut089z_finalize(uint16_t nwk_addr, uint8_t dst_ep) {
    const bool z = miboxer_set_zones(nwk_addr, dst_ep);
    const bool s = miboxer_tuya_setup(nwk_addr, dst_ep);
    return z && s;
}

// zigbee_pool_remove implementation moved to zigbee_pool.cpp where the
// underlying storage (s_pool / s_pool_count) lives. Direct extern access
// was removed from zigbee_pool.h to enforce the mutex contract.
