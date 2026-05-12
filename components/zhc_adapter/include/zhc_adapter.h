// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// ZHAC ↔ embedded/zhc/ adapter.
//
// Primary decode path for every device the zhc library can match. On a
// successful match the adapter logs the decoded payload and forwards
// each key/value through the shadow bridge (`zhac_adapter_register_shadow`)
// so values reach device_shadow + NVS. `zhac_adapter_try_decode` returns
// true when the library claimed the frame — callers use that as a
// "skip legacy fallback" signal so the old `zcl_converter` pipeline
// doesn't double-publish to the same device.
//
// Write path: `zhac_adapter_send_*` encodes a ZCL frame via the
// device's TzConverter array and hands it to the radio hook
// (`zhac_adapter_register_send`).
//
// Safe to call on every frame. Non-matching devices return quickly.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-shot init — call from app_main once.
void zhac_adapter_init(void);

// Returns true if ZHC has a prepared definition matching the given
// modelId + manufacturerName, or — when `ieee != 0` — a synthetic
// cluster-based fallback is available for that device. Caller code
// uses this to classify a device as MATCHED/UNMATCHED (see
// zigbee_identity / zigbee_interview) without having to walk the
// library tables themselves. Pass ieee=0 for registry-only lookup
// (e.g. in a path where the device record isn't available yet).
bool zhac_adapter_has_def(uint64_t ieee,
                           const char* model_id,
                           const char* manufacturer_name);

// Record the input/output cluster list parsed from a device's
// Simple_Desc_rsp. Called once per endpoint during the interview.
// Enables cluster-aware fallback exposes for devices that have no
// matching library definition (standard ZCL devices like the Arduino
// Zigbee range extender). Pass the raw ZCL cluster ids; the adapter
// maps them to the appropriate FZ/TZ converters + reporting specs.
void zhac_adapter_register_endpoint(uint64_t ieee,
                                     uint8_t  endpoint,
                                     uint16_t profile_id,
                                     uint16_t device_id,
                                     const uint16_t* in_clusters,
                                     size_t   n_in_clusters,
                                     const uint16_t* out_clusters,
                                     size_t   n_out_clusters);

// Drop fallback cluster data for this IEEE. Called by device-removal
// paths so a re-pair doesn't reuse stale endpoint info.
void zhac_adapter_fallback_clear(uint64_t ieee);

// Try to decode a single inbound ZCL frame with the new library.
// Returns true when the library both matched a prepared definition
// and produced ≥1 attribute. Output is logged at INFO level; nothing
// flows into device_shadow / the event bus.
//
// - ieee:             device IEEE (for logging only).
// - model_id:         Basic cluster modelIdentifier; null / empty → skip.
// - manufacturer_name: Basic cluster manufacturerName; may be null.
//                     Required to discriminate Tuya `TS0601 +
//                     _TZE200_xxx` devices which share the same
//                     modelId. Lumi-style devices ignore this.
// - cluster_id:       from AfRawFrame.
// - src_endpoint:     from AfRawFrame.
// - linkquality:      from AfRawFrame.
// - zcl:              pointer to the ZCL body (byte 0 = frame control).
// - zcl_len:          bytes available from zcl.
bool zhac_adapter_try_decode(uint64_t ieee,
                              const char* model_id,
                              const char* manufacturer_name,
                              uint16_t group_id,
                              uint16_t cluster_id,
                              uint8_t src_endpoint,
                              uint8_t linkquality,
                              const uint8_t* zcl, size_t zcl_len);

// ── Send path ────────────────────────────────────────────────────────
//
// The adapter owns frame encoding (via the zhc library's TzConverters)
// but not the radio. Platform code registers a transmit hook at init
// time; the adapter calls it with a fully-prepared ZCL frame (header +
// payload) and the destination addressing. Hook is nullable — send
// calls are silent no-ops until one is registered.
//
// TSN should be patched by the hook before transmission (byte 1 of
// `zcl_data`); the encoder writes 0x00 as a placeholder.
typedef bool (*zhac_af_send_fn_t)(uint16_t nwk_addr, uint8_t dst_ep,
                                   uint16_t cluster_id,
                                   const uint8_t* zcl_data, size_t zcl_len);

void zhac_adapter_register_send(zhac_af_send_fn_t fn);

// Tell the adapter the (ieee, nwk) tuple for the frame it's about to
// decode. The radio bridge calls this once per inbound APS frame
// before invoking `zhac_adapter_try_decode`. Required so fz
// converters that emit a response (Tuya MCU sync time, Zosung IR
// runtime) can hand the cluster_id + cmd_id + payload to the existing
// configure_cmd hook with a valid destination. No-op when paired with
// `zhac_adapter_try_decode` of an unrelated ieee.
void zhac_adapter_set_runtime_addr(uint64_t ieee, uint16_t nwk);

// ── Shadow bridge ────────────────────────────────────────────────────
//
// After a successful decode the adapter invokes this hook once per
// key/value in the output payload. Platform code (zigbee_mgr) resolves
// the string key verbatim into a ZclAttribute, and feeds it into
// device_shadow_process.
//
// value_kind encoding mirrors zhc::ValueType — adapter users don't need
// to pull in the C++ enum; values are: 1=Bool, 2=Uint, 3=Int, 4=Float,
// 5=StringRef. Unused numeric fields are set to 0; str_val is null when
// not applicable.
typedef void (*zhac_shadow_update_fn_t)(uint64_t ieee,
                                          const char* key,
                                          uint8_t value_kind,
                                          int64_t int_val,
                                          uint64_t uint_val,
                                          float float_val,
                                          bool bool_val,
                                          const char* str_val);

void zhac_adapter_register_shadow(zhac_shadow_update_fn_t fn);

// ── Configure hooks ──────────────────────────────────────────────────
//
// Called by `zhc::run_configure(def, ctx)` at device join. Platforms
// register ZDO_BIND + configureReporting transports; the zhc runtime
// walks `def.bindings[]` / `def.reports[]` and fires these callbacks
// per entry. Both are nullable — run_configure is a no-op if unset.
typedef bool (*zhac_configure_bind_fn_t)(uint64_t ieee,
                                           uint8_t  endpoint,
                                           uint16_t cluster_id);
typedef bool (*zhac_configure_report_fn_t)(uint64_t ieee,
                                             uint8_t  endpoint,
                                             uint16_t cluster_id,
                                             uint16_t attr_id,
                                             uint8_t  attr_type,
                                             uint16_t min_interval,
                                             uint16_t max_interval,
                                             uint32_t reportable_change,
                                             uint16_t manufacturer_code);

void zhac_adapter_register_configure(zhac_configure_bind_fn_t   bind,
                                       zhac_configure_report_fn_t report);

// Look up a device's ZHC definition by (model_id, manufacturer_name)
// and copy the friendly vendor + model labels into the supplied
// buffers. Returns true when a def was found. On false the buffers
// are left untouched so callers can emit empty strings.
//
// Used by the hap_json device encoder to surface "Miboxer / FUT089Z"
// in the web UI instead of "TS1002 / _TZ3000_xwh1e22x".
bool zhac_adapter_resolve_labels(const char* model_id,
                                   const char* manufacturer_name,
                                   char* vendor_out, size_t vendor_cap,
                                   char* model_out,  size_t model_cap);

// Build a JSON array of the matching device's `exposes` into `buf`
// (e.g. `[{"name":"state","type":"binary","access":3}, …]`). Returns
// bytes written (excluding NUL) or 0 on miss / overflow. The
// `access` bitmask follows the z2m convention: bit0 = STATE
// (publishes reports), bit1 = SET (writable), bit2 = GET (explicit
// read supported). Consumers use it to render read-only labels vs.
// editable controls without a hardcoded allowlist.
// `ieee` enables the cluster-based fallback for devices with no
// library definition — pass 0 to force registry-only. The returned
// JSON shape is identical in both cases so UI code stays simple.
size_t zhac_adapter_build_exposes_json(uint64_t ieee,
                                         const char* model_id,
                                         const char* manufacturer_name,
                                         char* buf, size_t cap);

// Drop the per-ieee cached ZHC definition pointer. Call after anything
// that could change the device's (model_id, manufacturer_name) tuple —
// late-identity fill, rename, rejoin with a different interview
// outcome — and when a device is removed from the pool so its slot
// doesn't serve a stale def to the next joiner.
void zhac_adapter_invalidate_def_cache(uint64_t ieee);

// ── Declarative configure-step hooks (v2) ────────────────────────────
//
// Platforms that support `def.config_steps[]` register these three
// additional transports. Null pointers leave the respective step ops
// as no-ops / auto-fail (Read, Cmd) or silent skips (Wait). Callers
// use the `ieee` to resolve the in-flight device in their own
// bookkeeping if needed; nwk is supplied for convenience since Z-Stack
// AF_DATA_REQUEST takes a short address.
typedef bool (*zhac_configure_cmd_fn_t)(uint64_t ieee, uint16_t nwk,
                                          uint8_t  endpoint,
                                          uint16_t cluster_id,
                                          uint8_t  cmd_id,
                                          const uint8_t* payload,
                                          uint8_t  payload_len,
                                          uint8_t  flags);

typedef bool (*zhac_configure_read_fn_t)(uint64_t ieee, uint16_t nwk,
                                           uint8_t  endpoint,
                                           uint16_t cluster_id,
                                           const uint8_t* attr_ids_le,
                                           uint8_t  attr_id_count,
                                           uint16_t manu_code);

typedef void (*zhac_configure_sleep_fn_t)(uint16_t wait_ms);

void zhac_adapter_register_configure_ex(zhac_configure_cmd_fn_t   cmd,
                                          zhac_configure_read_fn_t  read,
                                          zhac_configure_sleep_fn_t sleep);

// Fire `run_configure` for a device that has just joined. `ieee`
// resolves to a PreparedDefinition via the same lookup path as
// `zhac_adapter_try_decode`. Returns true if every bind + report
// accepted. Callers typically invoke this after the interview
// completes and the model/manu is known.
bool zhac_adapter_configure(uint64_t ieee, uint16_t nwk,
                              const char* model_id,
                              const char* manufacturer_name);

// Write-path wrappers. `model_id` / `manufacturer_name` resolve the
// PreparedDefinition just like the decode path. Returns false when no
// TzConverter claims `key`, the encoder fails, or the radio hook isn't
// registered.
bool zhac_adapter_send_bool(uint64_t ieee,
                             const char* model_id,
                             const char* manufacturer_name,
                             uint16_t nwk_addr, uint8_t dst_endpoint,
                             const char* key, bool value);
bool zhac_adapter_send_uint(uint64_t ieee,
                             const char* model_id,
                             const char* manufacturer_name,
                             uint16_t nwk_addr, uint8_t dst_endpoint,
                             const char* key, uint64_t value);
bool zhac_adapter_send_string(uint64_t ieee,
                               const char* model_id,
                               const char* manufacturer_name,
                               uint16_t nwk_addr, uint8_t dst_endpoint,
                               const char* key, const char* value);

#ifdef __cplusplus
}  // extern "C"
#endif
