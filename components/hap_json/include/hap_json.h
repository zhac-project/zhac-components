// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/hap_json/include/hap_json.h
#pragma once
#include "zap_common.h"
#include "zcl_attribute.h"
#include "hap_protocol.h"
#include <cstdint>
#include <cstddef>

// ── String escaping helper ───────────────────────────────────────────────
// Escape a NUL-terminated byte string for inclusion as a JSON string
// value (without surrounding quotes). Writes into out[0..out_cap-1] and
// always null-terminates. Truncates cleanly when the escaped form would
// overflow `out_cap`. Escapes `"`, `\`, `\n`, `\r`, `\t`, and other
// control bytes (< 0x20) per RFC 8259 (control bytes other than the
// named ones are dropped — same policy as ws_bridge's pre-existing
// helper). Returns the number of bytes written (excluding the NUL).
size_t hap_json_escape_str(const char* src, char* out, size_t out_cap);

// ── SYNC ──────────────────────────────────────────────────────────────────
struct HapSyncInfo {
    uint32_t session_id;
    char     fw_ver[32];     // git-describe version (tag, or tag-commits-ghash)
    uint16_t device_count;   // only in ACK
    bool     is_ack;         // true if SYNC_ACK, false if SYNC_REQ
};
bool hap_json_encode_sync_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                               uint32_t session_id, const char* fw_ver);
bool hap_json_encode_sync_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                               uint32_t session_id, const char* fw_ver, uint16_t device_count);
bool hap_json_decode_sync(const uint8_t* payload, uint16_t len, HapSyncInfo& out);

// ── HEARTBEAT ─────────────────────────────────────────────────────────────
struct HapHeartbeat {
    uint32_t uptime;       // seconds since boot
    uint8_t  cpu_pct_c0;   // CPU usage core 0, 0-100 (0 if unknown)
    uint8_t  cpu_pct_c1;   // CPU usage core 1, 0-100 (0 if unknown)
    uint32_t heap;         // esp_get_free_heap_size() — total across all caps
    uint32_t psram_free;   // free PSRAM bytes (0 if no PSRAM)
    uint32_t psram_total;  // total PSRAM bytes (0 if no PSRAM)
    uint32_t proto_mask;   // bitmask: bit0=Zigbee, bit1=BLE, bit2=Thread, ...
    // Extended memory diagnostics (added 2026-04-22). Older senders
    // omit these keys and decoders default them to 0.
    uint32_t heap_min_free;           // esp_get_minimum_free_heap_size()
    uint32_t internal_free;           // free internal-RAM bytes (MALLOC_CAP_INTERNAL)
    uint32_t internal_min_free;       // minimum free internal RAM since boot
    uint32_t internal_largest_block;  // largest contiguous internal-RAM block
    uint32_t psram_min_free;          // minimum free PSRAM since boot
    uint32_t psram_largest_block;     // largest contiguous PSRAM block
    uint32_t task_stack_hwm_bytes;    // smallest high-water-mark across all tasks
    // Live device count (P4 zigbee_pool::pool_count). Older receivers
    // ignore the field; this is the canonical refresh path for
    // `s_p4_device_count` after the boot-time SYNC ACK.
    uint16_t device_count;
};
bool hap_json_encode_heartbeat(uint8_t* buf, size_t cap, uint16_t* out_len,
                                const HapHeartbeat& hb);
bool hap_json_decode_heartbeat(const uint8_t* payload, uint16_t len, HapHeartbeat& out);

// ── GET_DEVICES / DEVICE_LIST ────────────────────────────────────────────
// Optional resolver used by the encoder to look up the friendly
// vendor + model labels from the ZHC definition catalogue. The
// encoder lives in a component shared by P4 and S3; S3 has no zhc
// library so it passes `nullptr` and the raw device-reported
// strings fall through. P4 wraps `zhac_adapter_resolve_labels`.
typedef void (*HapJsonLabelResolverFn)(const ZapDevice* dev,
                                         char* vendor_out, size_t vendor_cap,
                                         char* model_out,  size_t model_cap);

// Optional emitters used by the device encoders. Each writes a complete
// JSON value (object for attrs, array for exposes) into the supplied
// buffer and returns the number of bytes written, NOT including a
// trailing NUL. Return 0 to skip the field. The full-info splicer
// reserves one byte for the re-closing `}` of the outer object, so an
// emitter that writes exactly `cap` bytes will be discarded — size
// strictly under `cap`.
typedef size_t (*HapJsonAttrsEmitter)(const ZapDevice* dev, char* buf, size_t cap);
typedef size_t (*HapJsonExposesEmitter)(const ZapDevice* dev, char* buf, size_t cap);

// Encode the device list as `{"devices":[ {...}, ... ]}`.
//
// PAGING (HOTFIX): a full fleet's JSON does not fit one SPI frame — a single
// HAP frame can never exceed HAP_MAX_PAYLOAD(4096), and ~16 realistic devices
// already blow that budget, which made the old all-or-nothing encoder return
// false → P4 logged "encode failed", sent nothing, and S3's roundtrip timed
// out. The encoder therefore pages: it fills `buf` device-by-device starting
// at `start_index`, stops BEFORE the frame would overflow, and reports the
// index of the first un-encoded device.
//
//   start_index : first device (post-skip indexing is by raw array index)
//                 to encode. 0 for the first page.
//   next_index  : OUT. Set to the raw array index of the first device that
//                 did NOT fit (the cursor to pass as the next start_index).
//                 Set to `count` when the page reached the end (done
//                 sentinel). When non-null the encoder runs in PAGED mode.
//                 When nullptr the encoder runs in legacy single-frame mode
//                 (encode all `count`, fail on overflow) for callers that
//                 still want the old all-or-nothing behaviour.
//
// Forward-progress guarantee: if even one device at `start_index` does not
// fit (pathological huge single record), it is still emitted alone and
// `next_index` is advanced past it (logged) — the function never returns
// `*next_index == start_index` with zero devices encoded, so a paging caller
// can never spin. Soft-removed devices are skipped but still consume an index
// so the cursor is monotonic over the raw array.
bool hap_json_encode_device_list(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const ZapDevice* devs, uint16_t count,
                                  HapJsonLabelResolverFn resolve = nullptr,
                                  uint16_t start_index = 0,
                                  uint16_t* next_index = nullptr);

// ── GET_DEVICE_BY_ID (0x12) / DEVICE_INFO (0x13) ─────────────────────────
// Request:  {"ieee":"0xXXXXXXXXXXXXXXXX"}
// Response: {"ieee":"0x...","nwk":N,"name":"...","type":N,"last_seen":N,
//            "mfr":N,"lqi":N,"bat_pct":N,"ep_count":N,
//            "eps":[N,...],"clusters":[[N,N,...],...],"ok":true}
//         or {"ok":false,"err":"not found"}
bool hap_json_encode_get_device_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     uint64_t ieee);
bool hap_json_decode_get_device_req(const uint8_t* payload, uint16_t len,
                                     uint64_t* ieee_out);
bool hap_json_encode_device_info(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const ZapDevice* dev,
                                  HapJsonLabelResolverFn resolve = nullptr);
bool hap_json_encode_device_info_err(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* err);

// Same as hap_json_encode_device_info but additionally splices
// `,"attrs":{...}` and/or `,"exposes":[...]` from the supplied
// emitters. Pass nullptr for either emitter to skip that field. The
// shadow-attrs and zhc-exposes producers live outside hap_json (P4
// only); this keeps the encoder layering clean while allowing one
// encode call per DEVICE_INFO send.
bool hap_json_encode_device_info_full(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const ZapDevice* dev,
                                       HapJsonLabelResolverFn resolve = nullptr,
                                       HapJsonAttrsEmitter   attrs   = nullptr,
                                       HapJsonExposesEmitter exposes = nullptr);

// ── DEVICE_SET_NAME (0x24, S3→P4) ────────────────────────────────────────
// Request:  {"ieee":"0xXXXX...","name":"friendly name"}
// Response: DEVICE_INFO (full updated device) or DEVICE_INFO_ERR on failure.
bool hap_json_encode_device_set_name(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      uint64_t ieee, const char* name);
bool hap_json_decode_device_set_name(const uint8_t* payload, uint16_t len,
                                      uint64_t* ieee_out, char* name_out, size_t name_max);

// ── GROUP_MEMBER_QUERY (0x16): {"ieee","ep"} ────────────────────────────────
bool hap_json_decode_group_query(const uint8_t* payload, uint16_t len,
                                 uint64_t* ieee_out, uint8_t* ep_out);

// ── DEVICE_JOIN (0x21) / DEVICE_LEAVE (0x22) ─────────────────────────────
// Payload: {"ieee":"0xXXXXXXXXXXXXXXXX"}
bool hap_json_encode_device_join(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint64_t ieee);
bool hap_json_decode_device_join(const uint8_t* payload, uint16_t len,
                                  uint64_t* ieee_out);

// ── SET_ATTRIBUTE ─────────────────────────────────────────────────────────
struct HapSetAttrReq {
    uint64_t ieee;
    uint8_t  ep;
    uint16_t cluster;
    uint16_t attr;
    int32_t  val;
    char     key[24];   // friendly attribute name (e.g. "state", "brightness")
};
bool hap_json_encode_set_attr(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const HapSetAttrReq& req);
bool hap_json_decode_set_attr(const uint8_t* payload, uint16_t len, HapSetAttrReq& out);
bool hap_json_encode_set_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                              uint64_t ieee, bool ok);

// ── ALERT (0x23) ──────────────────────────────────────────────────────────
// P4→S3: {"code":N,"ieee":"0x...","msg":"...","ts":N}
enum class HapAlertCode : uint8_t {
    LOW_BATTERY  = 1,
    LQI_LOW      = 2,
    SCRIPT_CRASH = 3,
    RULE_ERROR   = 4,
    SYSTEM       = 5,
};

struct HapAlert {
    HapAlertCode code;
    uint64_t     ieee;   // 0 if not device-specific
    char         msg[80];
    uint32_t     ts;
};
bool hap_json_encode_alert(uint8_t* buf, size_t cap, uint16_t* out_len,
                            const HapAlert& a);
bool hap_json_decode_alert(const uint8_t* payload, uint16_t len,
                            HapAlert& out);

// ── DEVICE_EVENT ──────────────────────────────────────────────────────────
struct HapDeviceEvent {
    uint64_t ieee;
    uint16_t cluster;
    uint16_t attr;
    int32_t  val;
    uint32_t ts;
    uint8_t  val_type;  // ValType — VAL_INT=1, VAL_BOOL=2, VAL_STR=3
};
bool hap_json_encode_device_event(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const HapDeviceEvent& ev);

// ── BULK_STATE_UPDATE (0x60) ──────────────────────────────────────────────
// Max 50 events per batch (4096-byte payload limit).
bool hap_json_encode_bulk(uint8_t* buf, size_t cap, uint16_t* out_len,
                           const HapDeviceEvent* evs, uint8_t count);

// Single-device attribute delta pushed live to the web-UI via WS. Shape:
//   {"type":"device_update","ieee":"0xXXXX...","attrs":{"<key>":<val>},
//    "lqi":<uint8>,"last_seen":<unix_ts>}
// `key` must be NUL-terminated. `val_type` uses ValType semantics
// (INT=1, BOOL=2, STR=3). For STR, pass `str_val` as the resolved string
// (NULL renders as JSON null). For BOOL/INT the value is taken from
// `int_val`. Returns false on overflow.
bool hap_json_encode_device_attr_update(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         uint64_t ieee, const char* key,
                                         uint8_t val_type, int32_t int_val,
                                         const char* str_val,
                                         uint8_t lqi, uint32_t last_seen);

// ── RULE management (0x30–0x35) ──────────────────────────────────────────

struct HapRuleExecResult {
    bool     ok;
    uint16_t rule_id;   // assigned on create; echoed on delete/update
    char     err[64];   // empty string on success
};

struct HapRuleSlotInfo {
    uint16_t rule_id;
    uint8_t  rule_type;  // 0 = SIMPLE (1 was BERRY — reserved, Berry removed)
    bool     enabled;
    char     name[24];   // friendly display name (NUL-terminated)
    char     src[128];   // DSL source (truncated if longer)
};

// RULE_CREATE (S3→P4): {"name":"<label>","dsl":"..."}
bool hap_json_encode_rule_create(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const char* name, const char* dsl);
bool hap_json_decode_rule_create(const uint8_t* payload, uint16_t len,
                                  char* name_out, size_t name_max,
                                  char* dsl_out,  size_t dsl_max);

// RULE_DELETE (S3→P4): {"id":5}
bool hap_json_encode_rule_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint16_t rule_id);
bool hap_json_decode_rule_delete(const uint8_t* payload, uint16_t len,
                                  uint16_t* rule_id_out);

// RULE_UPDATE (S3→P4): {"id":5,"enabled":true}
bool hap_json_encode_rule_update(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint16_t rule_id, bool enabled);
bool hap_json_decode_rule_update(const uint8_t* payload, uint16_t len,
                                  uint16_t* rule_id_out, bool* enabled_out);

// RULE_UPDATE_DSL (S3→P4): {"id":5,"name":"<label>","dsl":"ON … DO … ENDON"}
bool hap_json_encode_rule_update_dsl(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      uint16_t rule_id, const char* name,
                                      const char* dsl);
bool hap_json_decode_rule_update_dsl(const uint8_t* payload, uint16_t len,
                                      uint16_t* rule_id_out,
                                      char* name_out, size_t name_max,
                                      char* dsl_out,  size_t dsl_max);

// RULE_LIST_REQ (S3→P4): {}
bool hap_json_encode_rule_list_req(uint8_t* buf, size_t cap, uint16_t* out_len);

// RULE_LIST_RSP (P4→S3): {"rules":[{"id":1,"type":0,"enabled":true,"src":"..."},...]}
bool hap_json_encode_rule_list_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    const HapRuleSlotInfo* slots, uint16_t count);
bool hap_json_decode_rule_list_rsp(const uint8_t* payload, uint16_t len,
                                    HapRuleSlotInfo* slots, uint16_t max_slots,
                                    uint16_t* count_out);

// RULE_EXEC_RESULT (P4→S3): {"ok":true,"id":5,"err":""}
bool hap_json_encode_rule_exec_result(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const HapRuleExecResult& r);
bool hap_json_decode_rule_exec_result(const uint8_t* payload, uint16_t len,
                                       HapRuleExecResult& out);

// ── SCRIPT management (0x36–0x3C) ────────────────────────────────────────
// SCRIPT_ACK (P4→S3) reuses HapRuleExecResult as a generic ok/err carrier
// (rule_id is unused for scripts — operations are correlated by HAP seq).
// SCRIPT_LIST_REQ (S3→P4) reuses hap_json_encode_rule_list_req ({}).

static constexpr uint16_t HAP_SCRIPT_MAX_SRC  = 3900; // leaves room for JSON wrapper
static constexpr uint16_t HAP_SCRIPT_NAME_MAX = 24;   // chars, excluding NUL

struct HapScriptInfo {
    char     name[HAP_SCRIPT_NAME_MAX + 1];
    uint16_t size;  // file size in bytes
};

// SCRIPT_WRITE (S3→P4): {"name":"motion","src":"...lua script..."}
bool hap_json_encode_script_write(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const char* name, const char* src);
bool hap_json_decode_script_write(const uint8_t* payload, uint16_t len,
                                   char* name_out, size_t name_max,
                                   char* src_out, size_t src_max);

// SCRIPT_DELETE (S3→P4): {"name":"motion"}
bool hap_json_encode_script_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    const char* name);
bool hap_json_decode_script_delete(const uint8_t* payload, uint16_t len,
                                    char* name_out, size_t name_max);

// SCRIPT_RUN_REQ (S3→P4): {"name":"motion"}
bool hap_json_encode_script_run_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     const char* name);
bool hap_json_decode_script_run_req(const uint8_t* payload, uint16_t len,
                                     char* name_out, size_t name_max);

// SCRIPT_CHECK_REQ (S3→P4): {"name":"motion","src":"<lua source>"}
// Identical payload shape to SCRIPT_WRITE; dedicated encoder/decoder so
// the two codepaths stay independent.
bool hap_json_encode_script_check_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       const char* name, const char* src);
bool hap_json_decode_script_check_req(const uint8_t* payload, uint16_t len,
                                       char* name_out, size_t name_max,
                                       char* src_out,  size_t src_max);

// SCRIPT_CHECK_RSP (P4→S3): {"ok":bool,"err":"<msg>","line":<int>}
bool hap_json_encode_script_check_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                       bool ok, const char* err, int line);
bool hap_json_decode_script_check_rsp(const uint8_t* payload, uint16_t len,
                                       bool* ok_out,
                                       char* err_out, size_t err_max,
                                       int*  line_out);

// SCRIPT_LIST_RSP (P4→S3): {"scripts":[{"name":"motion","size":128},...]}
bool hap_json_encode_script_list_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const HapScriptInfo* scripts, uint16_t count);
bool hap_json_decode_script_list_rsp(const uint8_t* payload, uint16_t len,
                                      HapScriptInfo* scripts, uint16_t max_scripts,
                                      uint16_t* count_out);

// SCRIPT_READ_REQ (S3→P4): {"name":"motion"}
bool hap_json_encode_script_read_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* name);
bool hap_json_decode_script_read_req(const uint8_t* payload, uint16_t len,
                                      char* name_out, size_t name_max);

// SCRIPT_READ_RSP (P4→S3): {"name":"motion","src":"...lua script..."}
bool hap_json_encode_script_read_rsp(uint8_t* buf, size_t cap, uint16_t* out_len,
                                      const char* name, const char* src);
bool hap_json_decode_script_read_rsp(const uint8_t* payload, uint16_t len,
                                      char* name_out, size_t name_max,
                                      char* src_out, size_t src_max);

// ── LOG_LINE (0x80) ───────────────────────────────────────────────────────
// P4→S3: {"msg":"<formatted log line>"}
// msg is the raw ESP-IDF formatted log string (may include colour codes).
static constexpr size_t HAP_LOG_MSG_MAX = 128;

bool hap_json_encode_log_line(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const char* msg);
bool hap_json_decode_log_line(const uint8_t* payload, uint16_t len,
                               char* msg_out, size_t msg_max);

// ── OTA_CHUNK (0x40) / OTA_STATUS (0x41) ─────────────────────────────────
// OTA_CHUNK payload is raw binary (not JSON):
//   [4 bytes: total_size][4 bytes: offset][1 byte: flags][3 bytes: pad][N bytes: data]
//   flags: 0x01 = last chunk
// OTA_STATUS payload is JSON: {"ok":bool,"rcvd":N,"total":N,"err":"..."}

struct HapOtaChunkHdr {
    uint32_t total;       // total firmware image size in bytes
    uint32_t offset;      // byte offset of this chunk
    uint8_t  flags;       // 0x01 = last chunk
    uint8_t  _pad[3];
};
static constexpr size_t HAP_OTA_CHUNK_HDR_SIZE = sizeof(HapOtaChunkHdr);  // 12
static constexpr size_t HAP_OTA_CHUNK_DATA_MAX = HAP_MAX_PAYLOAD - HAP_OTA_CHUNK_HDR_SIZE;

struct HapOtaStatus {
    bool     ok;
    uint32_t rcvd;
    uint32_t total;
    char     err[64];
};
bool hap_json_encode_ota_status(uint8_t* buf, size_t cap, uint16_t* out_len,
                                 const HapOtaStatus& s);
bool hap_json_decode_ota_status(const uint8_t* payload, uint16_t len,
                                 HapOtaStatus& out);

// ── PERMIT_JOIN (0x25, S3→P4) ────────────────────────────────────────────
// {"duration":N}   N=0 closes window
bool hap_json_encode_permit_join(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  uint8_t duration_s);
bool hap_json_decode_permit_join(const uint8_t* payload, uint16_t len,
                                  uint8_t* duration_out);

// ── BIND_REQ (0x27, S3→P4) / BIND_ACK (0x28, P4→S3) ─────────────────────
// Request:  {"src_ieee":"0x...","src_ep":N,"cluster":N,"dst_ieee":"0x...","dst_ep":N,"unbind":false}
// Response: {"ok":true/false}
struct HapBindReq {
    uint64_t src_ieee;
    uint8_t  src_ep;
    uint16_t cluster;
    uint64_t dst_ieee;
    uint8_t  dst_ep;
    bool     unbind;   // true = ZDO_UNBIND_REQ
};
bool hap_json_encode_bind_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                               const HapBindReq& r);
bool hap_json_decode_bind_req(const uint8_t* payload, uint16_t len,
                               HapBindReq& out);
bool hap_json_encode_bind_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                               bool ok);

// ── DEVICE_DELETE (0x29, S3→P4) / DEVICE_DELETE_ACK (0x2A, P4→S3) ────────
// Request:  {"ieee":"0x...","hard":bool?}
// Response: {"ok":true/false}
// `hard==false` (default): soft-remove — clear the pool slot but keep the
//   NVS record so a rejoin can restore configure state without rediscovery.
// `hard==true`: drop the NVS record too. UI "forget forever" button.
bool hap_json_encode_device_delete(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    uint64_t ieee, bool hard = false);
bool hap_json_decode_device_delete(const uint8_t* payload, uint16_t len,
                                    uint64_t* ieee_out, bool* hard_out = nullptr);
bool hap_json_encode_device_delete_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                        bool ok);

// ── INTERVIEW_REQ (0x2B, S3→P4) ──────────────────────────────────────────
// Fire-and-forget: P4 re-runs the full interview sequence for the device.
// Payload: {"ieee":"0x..."}  (same shape as DEVICE_DELETE)
bool hap_json_encode_interview_req(uint8_t* buf, size_t cap, uint16_t* out_len,
                                    uint64_t ieee);
bool hap_json_decode_interview_req(const uint8_t* payload, uint16_t len,
                                    uint64_t* ieee_out);

// ── DEVICE_OPTIONS_SET (0x2C, S3→P4) / DEVICE_OPTIONS_SET_ACK (0x2D) ─────
// Request:  {"ieee":"0x...", "occupancy_timeout":N?, "debounce_ms":N?, "throttle_ms":N?}
// Response: {"ok":true/false}
//
// `occupancy_timeout_s` / `debounce_ms` / `throttle_ms` are optional. Pass
// `nullptr` to the encoder to omit a field, and the decoder writes -1 into the
// out pointer when the field is absent — callers must skip forwarding to the
// shadow in that case.
bool hap_json_encode_device_options_set(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         uint64_t ieee,
                                         const int32_t* occupancy_timeout_s,
                                         const int32_t* debounce_ms,
                                         const int32_t* throttle_ms);
bool hap_json_decode_device_options_set(const uint8_t* payload, uint16_t len,
                                         uint64_t* ieee_out,
                                         int32_t* occupancy_timeout_s_out,
                                         int32_t* debounce_ms_out,
                                         int32_t* throttle_ms_out);
bool hap_json_encode_device_options_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                         bool ok);
bool hap_json_decode_device_options_ack(const uint8_t* payload, uint16_t len,
                                         bool* ok_out);

// ── ZIGBEE_CFG_SET (0x54, S3→P4) / ZIGBEE_CFG_SET_ACK (0x55, P4→S3) ───
// Operator-configured Zigbee identity. Request may carry any subset
// of {channel, net_key_hex, regenerate}. `regenerate=true` tells P4
// to generate a fresh random key server-side (overrides net_key_hex
// if both supplied). Changes are persisted to NVS; applying requires
// a subsequent factory reset.
bool hap_json_encode_zigbee_cfg_set(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     int8_t channel, const char* net_key_hex,
                                     bool regenerate);
bool hap_json_decode_zigbee_cfg_set(const uint8_t* payload, uint16_t len,
                                     int8_t* channel_out,
                                     uint8_t* net_key_out, size_t net_key_cap,
                                     bool* net_key_present_out,
                                     bool* regenerate_out);
bool hap_json_encode_zigbee_cfg_ack(uint8_t* buf, size_t cap, uint16_t* out_len,
                                     bool ok, uint8_t channel, bool net_key_set);
bool hap_json_decode_zigbee_cfg_ack(const uint8_t* payload, uint16_t len,
                                     bool* ok_out, uint8_t* channel_out,
                                     bool* net_key_set_out);

// ── MQTT_MSG_IN (0x3E, S3→P4) ────────────────────────────────────────────
// Forwards an incoming MQTT message to P4 so Lua scripts / rules can react.
struct HapMqttMsgIn {
    char topic[64];    // MQTT topic
    char payload[256]; // MQTT payload (truncated to fit)
};
bool hap_json_encode_mqtt_msg_in(uint8_t* buf, size_t cap, uint16_t* out_len,
                                  const char* topic, const char* payload);
bool hap_json_decode_mqtt_msg_in(const uint8_t* buf, uint16_t len,
                                  HapMqttMsgIn& out);

// ── TIME_SYNC (0x3F, S3→P4) ─────────────────────────────────────────────
// S3 sends current Unix timestamp after SNTP sync; P4 calls settimeofday().
// Re-sent hourly. Payload: {"ts":<uint32>}
bool hap_json_encode_time_sync(uint8_t* buf, size_t cap, uint16_t* out_len, uint32_t ts);
bool hap_json_decode_time_sync(const uint8_t* payload, uint16_t len, uint32_t* ts_out);

// ── MQTT_PUBLISH (0x70) ───────────────────────────────────────────────────
struct HapMqttPublish {
    char    topic[128];
    char    payload[512];
    uint8_t qos;
    bool    retain;
};
bool hap_json_encode_mqtt_publish(uint8_t* buf, size_t cap, uint16_t* out_len,
                                   const HapMqttPublish& msg);
bool hap_json_decode_mqtt_publish(const uint8_t* payload, uint16_t len,
                                   HapMqttPublish& out);

// ── Telegram (binary-packed, not JSON) ────────────────────────────────────
struct HapTgSettoken { char token[97]; uint8_t token_len; };  // token NUL-term in C
struct HapTgSetchat  { char chat[33];  uint8_t chat_len; };
struct HapTgSend {
    char     chat[33];        uint8_t chat_len;        // empty = use default
    char     parse_mode[33];  uint8_t parse_mode_len;  // empty = no parse_mode
    char     text[3072];      uint16_t text_len;       // up to 3 KB so it
                                                       // never exceeds HAP_MAX_PAYLOAD
                                                       // even with metadata.
};

bool hap_pack_tg_settoken(uint8_t* buf, size_t cap, uint16_t* out_len,
                           const HapTgSettoken& m);
bool hap_unpack_tg_settoken(const uint8_t* buf, uint16_t len, HapTgSettoken* out);

bool hap_pack_tg_setchat(uint8_t* buf, size_t cap, uint16_t* out_len,
                          const HapTgSetchat& m);
bool hap_unpack_tg_setchat(const uint8_t* buf, uint16_t len, HapTgSetchat* out);

bool hap_pack_tg_send(uint8_t* buf, size_t cap, uint16_t* out_len,
                       const HapTgSend& m);
bool hap_unpack_tg_send(const uint8_t* buf, uint16_t len, HapTgSend* out);
