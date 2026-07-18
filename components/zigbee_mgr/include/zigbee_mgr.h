// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/include/zigbee_mgr.h
#pragma once
#include "zap_common.h"
#include <cstdint>

// Initialise zigbee_mgr + znp_driver + run ZNP coordinator init sequence.
// Blocks until coordinator is up or error. Returns true on success.
bool zigbee_mgr_init();

// Open/close permit join window (0 = close, 255 = open indefinitely).
bool zigbee_permit_join(uint8_t duration_s);

// Send ZCL On/Off command to a device endpoint.
// cluster must be 0x0006. cmd: 0x00=Off, 0x01=On, 0x02=Toggle.
bool zigbee_zcl_on_off(uint16_t nwk_addr, uint8_t ep, uint8_t cmd);

// Add / remove a device endpoint's ZCL Groups (0x0004) membership. A member of
// group N obeys commands (incl. hardware zone-remote groupcasts) sent to N.
// Delivery-confirmed only; authoritative state via Get Group Membership.
bool zigbee_zcl_group_add(uint16_t nwk_addr, uint8_t ep, uint16_t group_id);
bool zigbee_zcl_group_remove(uint16_t nwk_addr, uint8_t ep, uint16_t group_id);

// Send ZCL Identify (0x0003 cmd 0x00): the device blinks/beeps for `seconds`.
bool zigbee_zcl_identify(uint16_t nwk_addr, uint8_t ep, uint16_t seconds);

// Send ZCL MoveToLevel (cluster 0x0008, cmd 0x04) to a device endpoint.
// level: 0–254. transition_tenths: transition time in 1/10 s (0 = immediate).
bool zigbee_zcl_level(uint16_t nwk_addr, uint8_t ep, uint8_t level,
                      uint16_t transition_tenths);

// Send ZCL MoveToColorTemperature (cluster 0x0300, cmd 0x0A) to a device endpoint.
// color_temp_mireds: typically 153 (6500K) – 500 (2000K).
// transition_tenths: transition time in 1/10 s (0 = immediate).
bool zigbee_zcl_color_temp(uint16_t nwk_addr, uint8_t ep, uint16_t color_temp_mireds,
                            uint16_t transition_tenths);

// Returns the coordinator's own IEEE address (fetched during init via SYS_GET_EXTADDR).
// Returns 0 if init has not completed yet.
uint64_t zigbee_mgr_coordinator_ieee();

// ZDO bind/unbind between two devices on a specific cluster.
bool zigbee_zdo_bind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                     uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep);
bool zigbee_zdo_unbind(uint16_t src_nwk, uint64_t src_ieee, uint8_t src_ep,
                       uint16_t cluster, uint64_t dst_ieee, uint8_t dst_ep);

// Send ZDO_MGMT_LEAVE_REQ to remove a device from the network.
bool zigbee_leave_req(uint16_t nwk_addr, uint64_t ieee);

// Re-run the full interview sequence for a device already in the pool.
// Returns true if the request was accepted (device exists + queued); false
// on unknown device or full queue. Callers should propagate the bool —
// callers used to assume scheduling always succeeded (CODEX §7).
bool zigbee_interview_trigger(uint64_t ieee);

// Drop any queued JoinEvents. Called by zigbee_mgr_reinit() during crash
// recovery so pre-crash join events don't re-fire with stale NWK addresses.
void zigbee_interview_flush_join_queue();

// Remove a device from the in-memory pool by IEEE address.
// Returns true if found and removed.
bool zigbee_pool_remove(uint64_t ieee);

// Returns true if an unexpected SYS_RESET_IND arrived after init (ZNP crash).
bool zigbee_mgr_crashed();

// Re-run coordinator startup without repeating one-time setup.
// Call from task_zigbee when zigbee_mgr_crashed() is true.
bool zigbee_mgr_reinit();

// Respond to a profile-wide Read Attributes on genTime (cluster 0x000A).
// Tuya end-devices (MiBoxer FUT089Z, many TS0601 remotes) loop on this
// read until they receive a real LocalTime (attr 0x0007) value — their
// firmware gates button reporting on a successful time sync. The
// responder supplies UTC-since-2000 (Zigbee epoch) for attr 0x0007 and
// marks every other requested attr as UNSUPPORTED so the device stops
// probing. Call from the AF_INCOMING_MSG dispatch when cluster 0x000A +
// CMD_READ_ATTR is observed.
bool zigbee_respond_gen_time(uint16_t nwk_addr, uint8_t dst_ep,
                              uint8_t trans_seq,
                              const uint8_t* request_body,
                              size_t request_body_len);

// Send a ZCL Default Response (global cmd 0x0B) back to a device. Per
// ZCL spec, devices set the "disable default response" bit in the
// outgoing frame control when they do NOT want an ACK; every other
// unicast frame they emit expects a default response, and Tuya sleepy
// end-devices retransmit up to 5× at APS level when one doesn't arrive.
// `incoming_fc` and `mfg_code` are echoed from the incoming frame so
// the response copies the mfg-specific bit + code correctly; direction
// is always inverted. Caller already filtered out groupcasts and
// default responses to avoid loops.
bool zigbee_send_default_response(uint16_t nwk_addr, uint8_t dst_ep,
                                   uint16_t cluster_id,
                                   uint8_t incoming_fc, uint16_t mfg_code,
                                   uint8_t tsn, uint8_t cmd_id,
                                   uint8_t status);

// Ship a fully-formed ZCL frame (`body` = FC|TSN|CMD|… already built by
// the caller; `trans_id` MUST equal the TSN at body[1] / body[3]) inside an
// AF_DATA_REQUEST to nwk_addr.dst_ep.cluster_id. Used by zhc_send_bridge so
// the single af_data_request() builder (and its no-blind-retry policy) is
// the ONE place that assembles AF headers (§4). `idempotent` selects retry
// behaviour: false (commands) = single send + AF_DATA_CONFIRM gate (no
// double-fire); true (reads/absolute writes) = multi-attempt SREQ retry.
// `confirm_timeout_ms` is the MAC-confirm wait for non-idempotent sends
// (0 = best-effort single send, no confirm). Returns true on accept (+
// confirmed delivery when gated).
bool zigbee_af_send_zcl(uint16_t nwk_addr, uint8_t dst_ep,
                        uint16_t cluster_id, uint8_t trans_id,
                        const uint8_t* body, uint32_t body_len,
                        bool idempotent, uint32_t confirm_timeout_ms);

// Generic ZCL Read Attributes. Sends a Read to
// `nwk_addr.endpoint.cluster_id` listing `attr_ids_le` — packed LE,
// two bytes per attribute, `attr_count` entries. When `manu_code` is
// non-zero the frame is built manufacturer-specific (FC bit 2 set,
// LE manu_code inserted between FC and TSN). Returns true when the
// SRSP status byte is 0 (command accepted by the ZNP).
bool zigbee_zcl_read(uint16_t nwk_addr, uint8_t endpoint,
                      uint16_t cluster_id,
                      const uint8_t* attr_ids_le, uint8_t attr_count,
                      uint16_t manu_code = 0);

// ZCL Configure Reporting (cmd 0x06) for ONE attribute. Builds a
// single-record send-direction frame:
//   direction=0x00 attr_id type min_interval max_interval reportable_change
// The reportable_change field width is derived from `attr_type` (1 B
// for u8/s8, 2 B for u16/s16, 4 B for u32/s32, omitted entirely for
// discrete types Bool/ENUM/bitmap). `manu_code` non-zero wraps the
// frame manufacturer-specific. Returns true when ZNP's SRSP status is
// 0; the device's per-attribute response lands later via AF_INCOMING
// and traverses the normal decode pipeline.
bool zigbee_zcl_configure_report(uint16_t nwk_addr, uint8_t endpoint,
                                  uint16_t cluster_id, uint16_t attr_id,
                                  uint8_t  attr_type,
                                  uint16_t min_interval,
                                  uint16_t max_interval,
                                  uint32_t reportable_change,
                                  uint16_t manu_code = 0);

// Generic ZCL Write Attributes (cmd 0x02) for a single attribute.
// `value` points at the on-wire byte buffer for `type` (e.g. 8 LE bytes
// for `0xF0`/IEEE Address). `manu_code` non-zero wraps the frame
// manufacturer-specific (FC=0x04, 5-byte header) exactly like
// `zigbee_zcl_read` — required by Aqara/lumi 0xFCC0 writes which z2m
// emits with `{manufacturerCode: 0x115f}` and which the hardware rejects
// if sent profile-wide. `manu_code == 0` (the default) keeps the legacy
// profile-wide 3-byte header (FC=0x00) for Tuya 0x8004, IAS CIE, etc.
// Returns true when the SRSP status byte is 0.
bool zigbee_zcl_write_attr(uint16_t nwk_addr, uint8_t endpoint,
                            uint16_t cluster_id, uint16_t attr_id,
                            uint8_t type, const uint8_t* value,
                            uint8_t value_len,
                            uint16_t manu_code = 0);

// Generic ZCL cluster-specific command. `flags` accepts
// `kStepFlag*` bits from `ConfigStep::flags` (only bit 0 =
// disable-default-response + bit 1 = manu-specific are honoured
// today; manu-specific is reserved — pass 0 unless you have a
// manufacturer code to add, which needs a future API extension).
bool zigbee_zcl_cluster_command(uint16_t nwk_addr, uint8_t endpoint,
                                 uint16_t cluster_id, uint8_t cmd_id,
                                 const uint8_t* payload, uint8_t payload_len,
                                 uint8_t flags);

// Variant that additionally blocks on AF_DATA_CONFIRM (MAC delivery
// status). Returns false on SRSP failure, MAC timeout, or non-zero
// MAC status. Use from the configure-step bridge so pipelines surface
// real delivery failures instead of silently proceeding past a
// dropped frame (the classic "configure DONE / device never emits
// anything" FUT089Z regression). Base variant stays fire-and-forget
// for the hot async path where a 2 s block per frame is too expensive.
bool zigbee_zcl_cluster_command_wait_confirm(uint16_t nwk_addr, uint8_t endpoint,
                                              uint16_t cluster_id, uint8_t cmd_id,
                                              const uint8_t* payload,
                                              uint8_t payload_len,
                                              uint8_t flags,
                                              uint32_t confirm_timeout_ms);

// Tuya "magic packet" — a fire-and-forget Read Attributes on genBasic
// for attrs [0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE]. Many
// Tuya-OEM devices (MiBoxer FUT089Z, TS0601 remotes) refuse to emit
// cluster-specific commands until they've seen this exact probe
// post-join. No response parsing is required — the device's internal
// state flips once the read is processed. Safe to issue on any
// device; harmless on non-Tuya stacks.
bool zigbee_tuya_magic_packet(uint16_t nwk_addr, uint8_t dst_ep);

// MiBoxer FUT089Z activation pair — sends the two custom commands z2m
// fires in its `miboxerFut089zControls` configure callback:
//   1. `genGroups` cmd 0xF0 `miboxerSetZones` — default 1:101..8:108
//      zone-to-group mapping (wire payload:
//      count u8 + 8× (groupId u16 LE, zoneNum u8)).
//   2. `genBasic` cmd 0xF0 `tuyaSetup` — empty payload, default
//      response suppressed.
// Without these the remote won't emit cluster-specific commands even
// after the generic Tuya magic packet. Returns true when BOTH commands
// land successfully at the ZNP.
bool zigbee_miboxer_fut089z_finalize(uint16_t nwk_addr, uint8_t dst_ep);

// Wipes the ZNP stick's ZNP_HAS_CONFIGURED marker so the NEXT boot
// runs full BDB commissioning (fresh network key + channel). Call
// from `zigbee_factory_reset()` before `esp_restart()` — otherwise
// the stick keeps its internal NVS (PAN ID, network key, device
// table) regardless of any ESP-side NVS wipes. Returns true on
// successful wipe.
bool zigbee_force_recommission();
