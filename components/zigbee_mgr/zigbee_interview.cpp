// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zigbee_interview.cpp
#include "znp_driver.h"
#include "zigbee_pool.h"
#include "device_shadow.h"
#include "event_bus.h"
#include "zap_store.h"
#include "zigbee_interview_utils.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>
#include <atomic>
#include "zcl_seq.h"
#include "zhc_adapter.h"
#include "zigbee_mgr.h"
#include "zigbee_configure_queue.h"
#include "task_stacks.h"

static const char* TAG = "zigbee_interview";

static uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

struct JoinEvent { uint64_t ieee; uint16_t nwk; };
static QueueHandle_t     s_join_queue = nullptr;
static SemaphoreHandle_t s_rsp_sem  = nullptr;
static MtFrame           s_rsp_frame;
// Interview-response scratch — filled in store_rsp() (non-ISR task
// context, signals via xSemaphoreGive). Sized to ZNP MT max payload.
// PSRAM-resident — interview path is infrequent; sequential memcpy fine.
EXT_RAM_BSS_ATTR static uint8_t s_rsp_buf[MT_MAX_PAYLOAD];
// std::atomic (not volatile) — volatile inhibits compiler reordering but
// not Xtensa LX7 CPU out-of-order. store_rsp() runs in the ZNP RX task and
// reads both fields back-to-back, so a publisher that armed the filter
// after flushing the semaphore must ensure the filter writes are visible
// before any ZDO AREQ delivered into store_rsp() — std::memory_order_release
// pairs with acquire-loads in store_rsp() for that handshake.
static std::atomic<uint8_t>  s_expected_rsp_cmd1{0xFF};   // 0xFF = not waiting
static std::atomic<uint16_t> s_expected_src_nwk {0xFFFF}; // 0xFFFF = no nwk filter

// Basic cluster read response interceptor
static SemaphoreHandle_t s_basic_sem = nullptr;
static uint8_t           s_basic_buf[128];
static uint8_t           s_basic_len = 0;
// CODEX M-02: std::atomic, not volatile — volatile guarantees neither atomicity
// nor cross-task ordering. The RX handler claims this single-shot (CAS to
// 0xFFFF) before touching s_basic_buf, mirroring the generic wait_rsp path.
static std::atomic<uint16_t> s_basic_expect_nwk{0xFFFF};  // 0xFFFF = not waiting
// Sleepy battery devices (Xiaomi sensors, Miboxer remotes) only wake
// for ~50-200 ms after a user action. 2 attempts × 3 s was missing the
// wake window. 5 × 5 s = up to 25 s per EP, giving multiple wake
// chances. The wake-triggered retry path further short-circuits the
// inter-attempt sleep when the device sends any AF frame.
static constexpr uint8_t BASIC_READ_RETRIES_PER_EP = 5;
static constexpr uint32_t BASIC_READ_TIMEOUT_MS = 5000;
static constexpr uint32_t BASIC_READ_RETRY_DELAY_MS = 400;

// Wake-triggered interview retry. Sleepy end-devices (Xiaomi cubes,
// Aqara buttons, TRVs) answer ZDO only while they're briefly awake.
// If an AF frame arrives from the device currently being interviewed
// the inter-attempt sleep in `task_interview` short-circuits and the
// next ZDO attempt fires immediately — ideally while the device is
// still awake. Producers: `zigbee_interview_on_af_incoming` (any AF
// ingress from active ieee/nwk) and `on_tc_dev_ind` (rejoin refresh).
static SemaphoreHandle_t s_wake_sem = nullptr;
// CODEX M-02: atomic, not volatile — a 64-bit ieee read on the 32-bit target
// can otherwise tear, and volatile provides no cross-task synchronisation.
static std::atomic<uint64_t> s_active_interview_ieee{0};
static std::atomic<uint16_t> s_active_interview_nwk {0xFFFE};

static inline void interview_wake_notify() {
    if (s_wake_sem) xSemaphoreGive(s_wake_sem);
}

static void store_rsp(const MtFrame& f) {
    uint8_t        want_cmd1 = s_expected_rsp_cmd1.load(std::memory_order_acquire);
    if (f.cmd1 != want_cmd1) return;            // wrong cmd1
    const uint16_t want_nwk  = s_expected_src_nwk.load(std::memory_order_acquire);
    if (want_nwk != 0xFFFF) {
        if (f.payload_len < 2) return;          // truncated
        uint16_t src = le16(f.payload);         // TI ZDO AREQ: src_nwk in [0..1] LE
        if (src != want_nwk) return;            // not our device
    }
    // §4 (FINDINGS.md, :96 SMELL): store_rsp runs in the ZNP RX task while
    // the interview task (other core) reads s_rsp_frame/s_rsp_buf after
    // wait_rsp returns. A duplicate matching ZDO AREQ arriving in the
    // window between the give below and the interview task disarming the
    // filter could memcpy into s_rsp_buf WHILE that buffer is being read —
    // a torn cross-core read. CLAIM the wait single-shot: atomically flip
    // the cmd1 filter to 0xFF (not-waiting). Only the thread that wins the
    // CAS fills the buffer and gives the sem; a racing duplicate now fails
    // the `f.cmd1 != want_cmd1` reload above (or the CAS) and is ignored,
    // so exactly one writer touches s_rsp_buf per arm.
    if (!s_expected_rsp_cmd1.compare_exchange_strong(
            want_cmd1, 0xFF, std::memory_order_acq_rel)) {
        return;   // another delivery already claimed this wait
    }
    s_rsp_frame.cmd0        = f.cmd0;
    s_rsp_frame.cmd1        = f.cmd1;
    s_rsp_frame.payload_len = f.payload_len;
    if (f.payload_len > 0 && f.payload)
        memcpy(s_rsp_buf, f.payload, f.payload_len);
    s_rsp_frame.payload = s_rsp_buf;
    xSemaphoreGive(s_rsp_sem);
}

static bool wait_rsp(uint8_t expected_cmd1, uint16_t expected_src_nwk,
                     uint32_t ms) {
    // Arm filter BEFORE flushing the semaphore. The opposite order leaves
    // a micro-window (one task slice + RX-FIFO drain ≈ 700 µs at 115200
    // baud) where ZDO AREQs are still gated by the previous wait's filter
    // (cmd1 still 0xFF after reset) and any matching response delivered
    // into store_rsp() is dropped. Symptom: spurious 3 s interview
    // timeouts under load.
    s_expected_rsp_cmd1.store(expected_cmd1,    std::memory_order_release);
    s_expected_src_nwk .store(expected_src_nwk, std::memory_order_release);
    xSemaphoreTake(s_rsp_sem, 0);   // flush any stale give from previous step
    bool ok = xSemaphoreTake(s_rsp_sem, pdMS_TO_TICKS(ms)) == pdTRUE;
    s_expected_rsp_cmd1.store(0xFF,   std::memory_order_release);
    s_expected_src_nwk .store(0xFFFF, std::memory_order_release);
    return ok;
}

// Intercept AF_INCOMING_MSG for Basic cluster read response during interview.
// Called from UART RX task via zigbee_mgr's on_af_incoming_msg forwarding.
void zigbee_interview_on_af_incoming(const uint8_t* payload, uint8_t payload_len) {
    if (payload_len < 6) return;
    const uint16_t src_nwk =
        static_cast<uint16_t>(payload[4]) |
        (static_cast<uint16_t>(payload[5]) << 8);

    // Wake signal — any AF ingress from the device currently under
    // interview shortens the inter-attempt sleep so the next ZDO
    // attempt rides the same awake window.
    if (src_nwk == s_active_interview_nwk.load(std::memory_order_acquire)) {
        interview_wake_notify();
    }

    uint16_t want_nwk = s_basic_expect_nwk.load(std::memory_order_acquire);
    if (want_nwk == 0xFFFF) return;            // not waiting on Basic
    if (payload_len < 17) return;

    const uint16_t cluster =
        static_cast<uint16_t>(payload[2]) |
        (static_cast<uint16_t>(payload[3]) << 8);
    if (cluster != 0x0000 || src_nwk != want_nwk) return;

    uint8_t data_len = payload[16];
    if (17u + data_len > payload_len) return;

    // CODEX M-02: claim the wait single-shot BEFORE writing the shared buffer,
    // atomically flipping the expectation to 0xFFFF. A second matching Basic
    // report otherwise overwrites s_basic_buf between this copy and the
    // interview task's parse (torn / stale read). Only the CAS winner fills the
    // buffer and gives the sem; a racing duplicate fails the CAS and is dropped.
    if (!s_basic_expect_nwk.compare_exchange_strong(
            want_nwk, 0xFFFF, std::memory_order_acq_rel)) {
        return;
    }
    s_basic_len = (data_len < sizeof(s_basic_buf)) ? data_len : sizeof(s_basic_buf);
    memcpy(s_basic_buf, payload + 17, s_basic_len);
    xSemaphoreGive(s_basic_sem);
}

// Read Basic cluster attributes (modelIdentifier + manufacturerName) via ZCL.
static bool interview_read_basic_once(ZapDevice* dev, uint16_t nwk, uint8_t ep) {
    // ZCL Read Attributes frame for cluster 0x0000:
    // [frame_ctrl=0x00 (global, client-to-server), seq, cmd=0x00 (ReadAttributes),
    //  attr_id_0=0x0004 (manufacturerName), attr_id_1=0x0005 (modelIdentifier)]
    uint8_t seq = zcl_seq_next();
    uint8_t zcl_frame[] = {
        0x00,           // frame control: global, client→server, disable default response
        seq,            // sequence number
        0x00,           // command: Read Attributes
        0x04, 0x00,     // attr 0x0004 (manufacturerName) LE
        0x05, 0x00,     // attr 0x0005 (modelIdentifier) LE
    };

    // AF_DATA_REQUEST payload
    uint8_t af_pl[10 + sizeof(zcl_frame)];
    af_pl[0] = nwk & 0xFF;
    af_pl[1] = (nwk >> 8) & 0xFF;
    af_pl[2] = ep;              // dst_ep
    af_pl[3] = 0x01;            // src_ep
    af_pl[4] = 0x00; af_pl[5] = 0x00;  // cluster 0x0000 (Basic) LE
    af_pl[6] = seq;             // trans_id
    af_pl[7] = 0x00;            // options
    af_pl[8] = 0x0F;            // radius
    af_pl[9] = sizeof(zcl_frame);
    memcpy(af_pl + 10, zcl_frame, sizeof(zcl_frame));

    // Arm the interceptor
    xSemaphoreTake(s_basic_sem, 0);
    s_basic_len = 0;
    s_basic_expect_nwk.store(nwk, std::memory_order_release);

    MtFrame req{};
    req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x01;  // AF_DATA_REQUEST
    req.payload = af_pl; req.payload_len = sizeof(af_pl);
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 2000, 2)) {
        ESP_LOGW(TAG, "Basic cluster read: AF_DATA_REQUEST failed");
        s_basic_expect_nwk.store(0xFFFF, std::memory_order_release);
        return false;
    }

    // Wait for response (up to 3s)
    if (xSemaphoreTake(s_basic_sem, pdMS_TO_TICKS(BASIC_READ_TIMEOUT_MS)) == pdTRUE && s_basic_len > 0) {
        // Accepts both Read Attributes Response (cmd 0x01) and Report
        // Attributes (cmd 0x0A). Sleepy Xiaomi endpoints send the latter
        // unsolicited right after rejoin.
        uint16_t mfg_code = dev->manufacturer_code;
        zigbee_parse_basic_identity(s_basic_buf, s_basic_len,
                                    dev->model_id, sizeof(dev->model_id),
                                    dev->manufacturer_name, sizeof(dev->manufacturer_name),
                                    &mfg_code);
        dev->manufacturer_code = mfg_code;
        ESP_LOGI(TAG, "Basic cluster: model_id='%s' manufacturer='%s'",
                 dev->model_id, dev->manufacturer_name);
        s_basic_expect_nwk.store(0xFFFF, std::memory_order_release);
        return dev->model_id[0] != '\0' || dev->manufacturer_name[0] != '\0';
    } else {
        ESP_LOGW(TAG, "Basic cluster read timeout for nwk=0x%04x ep=%u", nwk, ep);
    }
    s_basic_expect_nwk.store(0xFFFF, std::memory_order_release);
    return false;
}

// Returns true if at least one endpoint answered the Basic read (model_id
// or manufacturer_name present). Caller uses this to transition the
// interview_state into IDENTITY_READY vs IDENTITY_PENDING.
static bool interview_read_basic(ZapDevice* dev, uint16_t nwk) {
    uint8_t probe_eps[8]{};
    const uint8_t probe_count = zigbee_interview_build_basic_probe_order(*dev, probe_eps, 8);

    for (uint8_t i = 0; i < probe_count; i++) {
        for (uint8_t attempt = 1; attempt <= BASIC_READ_RETRIES_PER_EP; attempt++) {
            if (interview_read_basic_once(dev, nwk, probe_eps[i])) return true;
            if (attempt < BASIC_READ_RETRIES_PER_EP) {
                vTaskDelay(pdMS_TO_TICKS(BASIC_READ_RETRY_DELAY_MS));
            }
        }
    }
    return false;
}

// Returns true on success, false if a ZDO step failed (caller may retry).
static bool do_interview(uint64_t ieee, uint16_t nwk) {
    // Find-or-create + initial state writes under one lock. Without the
    // advisory lock, a concurrent `zigbee_pool_remove(ieee)` from a user
    // delete can land between the two formerly-separate lookups (TOCTOU):
    // call 1 returns non-null → is_rejoin=true, call 2 returns null → we
    // fall through to pool_add() but still treat the slot as a rejoin and
    // skip the support_state reset. The single locked lookup also halves
    // the hash work.
    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    bool is_rejoin = (dev != nullptr);
    if (!dev) dev = pool_add();
    if (!dev) {
        zigbee_pool_unlock();
        return false;
    }
    dev->ieee_addr = ieee;
    dev->nwk_addr  = nwk;
    // Reset interview progress for this attempt. support_state is preserved
    // across rejoins when the IEEE matches — a device we already know how
    // to drive does not lose its converter binding if it rejoins (Q2).
    dev->interview_state    = static_cast<uint8_t>(InterviewState::NONE);
    dev->configure_state    = static_cast<uint8_t>(ConfigureState::PENDING);
    dev->configure_attempts = 0;
    if (!is_rejoin) {
        dev->support_state  = static_cast<uint8_t>(SupportState::UNKNOWN);
    }
    // F6/F35 (FINDINGS.md) — RESOLVED: the multi-second ZDO/Basic pipeline
    // below used to dereference `dev` across blocking ZNP I/O after this
    // unlock; a concurrent swap-with-last pool_remove could relocate the
    // slot and a late write would land on the wrong record. The pipeline
    // now runs entirely on `work`, a detached stack copy taken under the
    // lock (`work` + the commit-block `snap` ≈ 1044 B of the 8 KiB
    // zb_interview stack), and the results are
    // committed field-by-field under a re-acquired lock at the end (see
    // the commit block) — skipped if the device vanished mid-interview.
    ZapDevice work = *dev;
    dev = nullptr;  // poison: nothing below may touch the live slot unlocked
    zigbee_pool_unlock();
    zigbee_pool_mark_dirty();  // nwk may have changed on rejoin — rebuild hash index

    if (is_rejoin)
        ESP_LOGI(TAG, "Device rejoin ieee=0x%016llx nwk=0x%04x", (unsigned long long)ieee, nwk);

    // 1. ZDO_NODE_DESC_REQ
    {
        uint8_t pl[4];
        pl[0] = nwk & 0xFF; pl[1] = (nwk >> 8) & 0xFF;
        pl[2] = pl[0]; pl[3] = pl[1];
        MtFrame req{};
        req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x02;
        req.payload = pl; req.payload_len = 4;
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 2000, 3)) {
            ESP_LOGE(TAG, "ZDO_NODE_DESC_REQ failed nwk=0x%04x", nwk);
            return false;
        }
        if (wait_rsp(0x82, nwk, 3000) && s_rsp_frame.payload_len >= 6) {
            work.device_type = s_rsp_frame.payload[4] & 0x07;
        }
    }

    // 2. ZDO_ACTIVE_EP_REQ
    // Sleepy battery devices (Miboxer remotes, Xiaomi sensors) often miss
    // the wake window for this ZDO step even when their ZCL paths work.
    // Match z2m's fallback: assume endpoint 1 if discovery fails. Basic
    // cluster reads on ep=1 typically succeed and let the interview
    // continue. Endpoint enumeration is upgraded later if a real reply
    // ever arrives via the wake-triggered retry path.
    uint8_t ep_list[8];
    uint8_t ep_count = 0;
    {
        uint8_t pl[4];
        pl[0] = nwk & 0xFF; pl[1] = (nwk >> 8) & 0xFF;
        pl[2] = pl[0]; pl[3] = pl[1];
        MtFrame req{};
        req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x05;
        req.payload = pl; req.payload_len = 4;
        MtFrame rsp{};
        bool zdo_ok = znp_sreq_retry(req, rsp, 2000, 3) &&
                      wait_rsp(0x85, nwk, 3000) &&
                      s_rsp_frame.payload_len >= 6;
        if (zdo_ok) {
            // §4 (FINDINGS.md, :311 SEC): the claimed endpoint count
            // (payload[5]) was stored as endpoint_count while the memcpy
            // separately clamped to the bytes actually present. On a
            // truncated ACTIVE_EP_RSP (claimed N but fewer than N endpoint
            // bytes on the wire) the trailing ep_list[] entries stayed
            // UNINITIALISED stack garbage yet were reported as real device
            // endpoints (then probed via SIMPLE_DESC, registered in the
            // adapter). Clamp the count to bytes-present (and to 8) BEFORE
            // anything consumes it, so endpoint_count never exceeds the
            // bytes we actually copied.
            const uint8_t claimed   = s_rsp_frame.payload[5];
            const uint16_t avail    = (s_rsp_frame.payload_len > 6)
                                        ? (s_rsp_frame.payload_len - 6) : 0;
            uint8_t safe_count = claimed;
            if (safe_count > 8)     safe_count = 8;
            if (safe_count > avail) safe_count = static_cast<uint8_t>(avail);
            ep_count = safe_count;
            if (ep_count) memcpy(ep_list, s_rsp_frame.payload + 6, ep_count);
            if (ep_count != claimed) {
                ESP_LOGW(TAG, "ACTIVE_EP_RSP claimed %u eps but only %u bytes "
                              "present nwk=0x%04x — clamped to %u",
                         claimed, (unsigned)avail, nwk, ep_count);
            }
        } else {
            ESP_LOGW(TAG, "ZDO_ACTIVE_EP_REQ failed nwk=0x%04x — assuming ep=1 (sleepy device fallback)", nwk);
            ep_list[0] = 1;
            ep_count = 1;
        }
    }

    work.endpoint_count = ep_count;
    memcpy(work.endpoints, ep_list, ep_count < 8 ? ep_count : 8);

    // Topology is known. Stay here until Basic read succeeds or times out —
    // later steps will promote to IDENTITY_READY or demote to IDENTITY_PENDING.
    work.interview_state = static_cast<uint8_t>(
        ep_count > 0 ? InterviewState::TOPOLOGY_READY : InterviewState::FAILED);

    // 3. ZDO_SIMPLE_DESC_REQ for each endpoint
    for (uint8_t e = 0; e < ep_count && e < 8; e++) {
        uint8_t pl[5];
        pl[0] = nwk & 0xFF; pl[1] = (nwk >> 8) & 0xFF;
        pl[2] = pl[0]; pl[3] = pl[1];
        pl[4] = ep_list[e];
        MtFrame req{};
        req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x04;
        req.payload = pl; req.payload_len = 5;
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 2000, 3) || !wait_rsp(0x84, nwk, 3000)) continue;

        const uint8_t* p = s_rsp_frame.payload;
        // TI Z-Stack SIMPLE_DESC_RSP payload (MT spec):
        //   [0..1] SrcAddr LE
        //   [2]    Status
        //   [3..4] NWKAddrOfInterest LE
        //   [5]    Length (of SimpleDesc that follows)
        //   [6]    Endpoint
        //   [7..8] AppProfileId LE
        //   [9..10] AppDeviceId LE
        //   [11]   AppDevVer/reserved (nibble/nibble)
        //   [12]   NumInClusters
        //   [13..13+2N-1] InClusterList (LE)
        //   [13+2N] NumOutClusters
        //   [13+2N+1 .. 13+2N+2M] OutClusterList (LE)
        constexpr uint8_t kInCntOff   = 12;
        constexpr uint8_t kInListOff  = 13;
        if (s_rsp_frame.payload_len < kInCntOff + 1) continue;
        const uint16_t profile_id = le16(p + 7);
        const uint16_t device_id  = le16(p + 9);
        const uint8_t  in_cnt     = p[kInCntOff];
        // Zero the slot first so stale positions from a previous device
        // (pool slot recycled on rejoin) don't leak into the fallback.
        for (uint8_t c = 0; c < ZAP_CLUSTERS_PER_EP; c++) {
            work.clusters[e][c]     = 0;
            work.clusters_out[e][c] = 0;
        }
        uint8_t parsed_in = 0;
        for (uint8_t c = 0; c < in_cnt && c < ZAP_CLUSTERS_PER_EP; c++) {
            if (kInListOff + c * 2 + 1 >= s_rsp_frame.payload_len) break;
            work.clusters[e][c] = le16(p + kInListOff + c * 2);
            parsed_in = c + 1;
        }
        // §4 (FINDINGS.md, :375 SEC): out_off was computed as a uint8_t, so
        // for an in-cluster count ≥ 122 the expression 13 + in_cnt*2 wraps
        // past 255 (e.g. in_cnt=122 → 257 → 1) and the out-cluster list is
        // parsed from the WRONG offset (back inside the in-cluster bytes).
        // in_cnt is attacker-influenced wire data, so use size_t math and an
        // explicit bound against the actual frame length.
        const size_t out_off = static_cast<size_t>(kInListOff) +
                               static_cast<size_t>(in_cnt) * 2u;
        uint8_t parsed_out = 0;
        if (out_off < s_rsp_frame.payload_len) {
            uint8_t out_cnt = p[out_off];
            for (uint8_t c = 0; c < out_cnt && c < ZAP_CLUSTERS_PER_EP; c++) {
                const size_t byte_off = out_off + 1u + static_cast<size_t>(c) * 2u;
                if (byte_off + 1 >= s_rsp_frame.payload_len) break;
                work.clusters_out[e][c] = le16(p + byte_off);
                parsed_out = c + 1;
            }
        }
        // Feed the cluster list into zhc_adapter so devices without a
        // library match get a cluster-aware fallback definition
        // (generic exposes + reports + initial reads). Copy through an
        // aligned local so we don't hand out pointers into the packed
        // ZapDevice struct (-Werror=address-of-packed-member).
        uint16_t in_copy[ZAP_CLUSTERS_PER_EP];
        uint16_t out_copy[ZAP_CLUSTERS_PER_EP];
        for (uint8_t c = 0; c < parsed_in;  ++c) in_copy[c]  = work.clusters[e][c];
        for (uint8_t c = 0; c < parsed_out; ++c) out_copy[c] = work.clusters_out[e][c];
        zhac_adapter_register_endpoint(ieee,
                                        ep_list[e],
                                        profile_id, device_id,
                                        in_copy,  parsed_in,
                                        out_copy, parsed_out);
    }

    // 4. Read Basic cluster attributes (modelIdentifier + manufacturerName).
    // Failure here is NOT terminal: device is still marked IDENTITY_PENDING
    // so a later inbound Basic report can promote it to IDENTITY_READY and
    // trigger re-match (Step 2 of the backend-agnostic lifecycle plan).
    work.model_id[0] = '\0';
    work.manufacturer_name[0] = '\0';
    bool basic_ok = false;
    if (ep_count > 0) {
        basic_ok = interview_read_basic(&work, nwk);
    }
    if (work.interview_state == static_cast<uint8_t>(InterviewState::TOPOLOGY_READY)) {
        work.interview_state = static_cast<uint8_t>(
            basic_ok ? InterviewState::IDENTITY_READY : InterviewState::IDENTITY_PENDING);
    }
    // Coalesced: intermediate save removed. A single zap_store_save_device
    // at the end of the interview covers the whole state transition and
    // halves NVS writes per join (one flash erase cycle saved).

    // On rejoin, evict stale shadow attr cache so old readings don't surface
    // before the device sends fresh attribute reports.
    if (is_rejoin) {
        device_shadow_clear_attrs(ieee);
    }

    // Match device and run configure steps. support_state is updated so the
    // matcher outcome is persisted alongside the device record and we do not
    // spam "no match" warnings on every rejoin.
    const uint8_t prev_support = work.support_state;
    const bool supported = zhac_adapter_has_def(ieee,
                                                 work.model_id,
                                                 work.manufacturer_name);
    if (supported) {
        work.support_state = static_cast<uint8_t>(SupportState::MATCHED);
        if (prev_support != work.support_state)
            ESP_LOGI(TAG, "definition matched: model='%s' mfg='%s'",
                     work.model_id, work.manufacturer_name);
        // Configure runs off-task via the configure queue: keeps the
        // interview task free and gets exponential-backoff retries + dedup.
        work.configure_state    = static_cast<uint8_t>(ConfigureState::PENDING);
        work.configure_attempts = 0;
    } else {
        work.support_state = static_cast<uint8_t>(SupportState::UNMATCHED);
        // Demote to DEBUG when identity is still pending — re-match is
        // expected once Basic traffic arrives. Otherwise log once per state
        // transition, not per rejoin.
        const bool identity_pending = (work.interview_state ==
            static_cast<uint8_t>(InterviewState::IDENTITY_PENDING));
        if (identity_pending) {
            ESP_LOGD(TAG, "no match yet nwk=0x%04x — awaiting identity", work.nwk_addr);
        } else if (prev_support != work.support_state) {
            ESP_LOGW(TAG, "no definition for nwk=0x%04x — device partially supported",
                     work.nwk_addr);
        }
    }

    // ── Commit (F6/F35) ───────────────────────────────────────────────
    // Re-acquire the lock, re-find by IEEE, and write the pipeline
    // results into the live slot FIELD-BY-FIELD — not `*live = work` —
    // so concurrent mutations that landed mid-interview survive:
    // nwk_addr (rejoin refresh via on_tc_dev_ind), friendly_name (user
    // rename), flags (leave/rejoin), last_seen/link_quality (zcl
    // liveness). If the device was hard-removed mid-interview, skip the
    // commit entirely — do not resurrect it.
    bool committed = false;
    ZapDevice snap;   // post-commit copy for the NVS mark outside the lock
    zigbee_pool_lock();
    if (ZapDevice* live = pool_find_by_ieee(ieee)) {
        live->device_type    = work.device_type;
        live->endpoint_count = work.endpoint_count;
        memcpy(live->endpoints,    work.endpoints,    sizeof(live->endpoints));
        memcpy(live->clusters,     work.clusters,     sizeof(live->clusters));
        memcpy(live->clusters_out, work.clusters_out, sizeof(live->clusters_out));
        live->manufacturer_code = work.manufacturer_code;
        // Late-identity guard: when our Basic read failed
        // (IDENTITY_PENDING) but the late-identity path
        // (zigbee_identity.cpp) already promoted the live slot to
        // IDENTITY_READY mid-interview, keep its identity + lifecycle
        // fields — matches the pre-refactor outcome where the late path
        // won this race (it re-matches + enqueues configure itself).
        const bool late_promoted =
            work.interview_state ==
                static_cast<uint8_t>(InterviewState::IDENTITY_PENDING) &&
            live->interview_state ==
                static_cast<uint8_t>(InterviewState::IDENTITY_READY);
        if (!late_promoted) {
            live->interview_state = work.interview_state;
            memcpy(live->model_id, work.model_id, sizeof(live->model_id));
            memcpy(live->manufacturer_name, work.manufacturer_name,
                   sizeof(live->manufacturer_name));
            live->support_state      = work.support_state;
            live->configure_state    = work.configure_state;
            live->configure_attempts = work.configure_attempts;
        }
        snap = *live;
        committed = true;
    }
    zigbee_pool_unlock();

    if (!committed) {
        ESP_LOGW(TAG, "interview finished but device vanished mid-interview "
                      "(removed) — results dropped ieee=0x%016llx",
                 (unsigned long long)ieee);
        return true;   // done; nothing left to persist, configure or announce
    }

    zap_store_mark_dirty(&snap, ZAP_PERSIST_HIGH);  // persist lifecycle state (user-visible)

    // Kick the deferred configure worker after persistence so the queued
    // work sees the committed support/configure state.
    if (snap.support_state == static_cast<uint8_t>(SupportState::MATCHED) &&
        snap.configure_state != static_cast<uint8_t>(ConfigureState::DONE)) {
        zigbee_configure_enqueue(ieee);
    }

    // Single-line lifecycle summary so the operator can tell at a glance
    // what state the device ended up in — especially when Basic read
    // timed out (interview=IDENTITY_PENDING) and the device is now
    // silently waiting on late identity promotion.
    ESP_LOGI(TAG, "interview done nwk=0x%04x ieee=0x%016llx "
                  "interview=%u support=%u configure=%u",
             snap.nwk_addr, (unsigned long long)ieee,
             snap.interview_state, snap.support_state, snap.configure_state);

    Event ev{};
    ev.type = EventType::DEVICE_JOIN;
    memcpy(ev.data, &ieee, sizeof(ieee));
    event_bus_publish(ev);
    return true;
}

// Sleepy end-devices (Aqara buttons, door sensors) only wake briefly —
// 3×5 s missed every wake window. Budget for 10×30 s covers typical
// Xiaomi 2–5 min wake cycles so the interview eventually catches a wake.
// Awake devices (Tuya dimmers, Philips bulbs) still complete on the
// first attempt.
static constexpr uint8_t  INTERVIEW_MAX_ATTEMPTS = 10;
static constexpr uint32_t INTERVIEW_RETRY_DELAY_MS = 30000;

static void task_interview(void*) {
    JoinEvent ev;
    for (;;) {
        if (xQueueReceive(s_join_queue, &ev, portMAX_DELAY) != pdTRUE) continue;

        // Rejoin fast-path: if the pool already has a fully-interviewed
        // record for this IEEE, skip the ZDO dance. Otherwise the second
        // JoinEvent (Xiaomi devices typically fire two TC_DEV_IND bursts
        // on wake) would reset interview_state=NONE and block dispatch
        // for the whole retry window while the device ignores ZDO.
        // F6/F35: lock spans the find + nwk fix-up so a concurrent
        // swap-with-last remove can't retarget `d` between the lookup
        // and the write. The enqueue/log/event run on copied values
        // after unlock.
        bool fast_path = false;
        uint16_t fp_nwk = 0;
        zigbee_pool_lock();
        if (ZapDevice* d = pool_find_by_ieee(ev.ieee)) {
            if (d->interview_state ==
                    static_cast<uint8_t>(InterviewState::IDENTITY_READY)) {
                if (d->nwk_addr != ev.nwk && ev.nwk != 0 &&
                    ev.nwk != 0xFFFE) {
                    d->nwk_addr = ev.nwk;
                    zigbee_pool_mark_dirty();   // recursive mutex — safe here
                }
                // Force a re-configure on rejoin. task_configure dedups
                // out any device already in ConfigureState::DONE, so the
                // zigbee_configure_enqueue() below would otherwise be a
                // silent no-op. A "DONE" only means the prior configure's
                // steps were *sent* without a transport error — not that
                // the device answered. If that device left mid-configure
                // (its initial zcl_reads hit a since-departed nwk) the
                // shadow never populated → web UI shows "no states". Reset
                // to PENDING here (atomically with the nwk fix-up) so the
                // re-enqueue genuinely re-runs binds + reporting + reads on
                // the new live nwk. Transient — not persisted; the worker
                // re-marks DONE/FAILED + persists the real outcome (mirrors
                // the "Mark IN_PROGRESS … not persisted" pattern). This is
                // gated to the rejoin fast-path only; the DONE-dedup still
                // protects every other enqueue caller from duplicate binds.
                if (d->configure_state == static_cast<uint8_t>(ConfigureState::DONE)) {
                    d->configure_state    = static_cast<uint8_t>(ConfigureState::PENDING);
                    d->configure_attempts = 0;
                }
                fp_nwk    = d->nwk_addr;
                fast_path = true;
            }
        }
        zigbee_pool_unlock();
        if (fast_path) {
            ESP_LOGI(TAG,
                     "Rejoin fast-path: interview already complete "
                     "ieee=0x%016llx nwk=0x%04x",
                     (unsigned long long)ev.ieee, fp_nwk);
            // Re-enqueue configure on rejoin. Device-side Tuya
            // magic-packet unlock + setZones / on-join bindings are
            // volatile: when the remote power-cycles and rejoins it
            // returns to locked state and emits nothing until we
            // walk the configure steps again. Skipping them (as we
            // did before this fix) explained the "joined but no
            // actions" reports on MiBoxer / Tuya remotes.
            zigbee_configure_enqueue(ev.ieee);
            Event out{};
            out.type = EventType::DEVICE_JOIN;
            uint64_t ieee = ev.ieee;
            memcpy(out.data, &ieee, sizeof(ieee));
            event_bus_publish(out);
            continue;   // next queued JoinEvent
        }

        s_active_interview_ieee.store(ev.ieee, std::memory_order_release);
        for (uint8_t attempt = 1; attempt <= INTERVIEW_MAX_ATTEMPTS; attempt++) {
            // Prefer the pool's current nwk over the nwk captured when
            // this event was queued. A rejoin that lands mid-retry (and
            // its ZDO_TC_DEV_IND update) is reflected in the pool via
            // `on_tc_dev_ind` below; without this re-read the retry
            // would target the device's old nwk and the ZDO response
            // would be dropped in `store_rsp` (src_nwk mismatch).
            uint16_t nwk = ev.nwk;
            zigbee_pool_lock();   // F6/F35: short locked read, copy out nwk
            if (const ZapDevice* d = pool_find_by_ieee(ev.ieee)) {
                if (d->nwk_addr != 0 && d->nwk_addr != 0xFFFE) {
                    nwk = d->nwk_addr;
                }
            }
            zigbee_pool_unlock();
            s_active_interview_nwk.store(nwk, std::memory_order_release);
            if (do_interview(ev.ieee, nwk)) break;
            if (attempt < INTERVIEW_MAX_ATTEMPTS) {
                ESP_LOGW(TAG, "Interview failed (attempt %d/%d) for ieee=0x%016llx nwk=0x%04x — retry in %lums (or on wake)",
                         attempt, INTERVIEW_MAX_ATTEMPTS,
                         (unsigned long long)ev.ieee, nwk,
                         (unsigned long)INTERVIEW_RETRY_DELAY_MS);
                // Flush any stale wake give from the aborted attempt so
                // the next call waits cleanly.
                xSemaphoreTake(s_wake_sem, 0);
                const TickType_t t0 = xTaskGetTickCount();
                const bool woken = xSemaphoreTake(
                    s_wake_sem,
                    pdMS_TO_TICKS(INTERVIEW_RETRY_DELAY_MS)) == pdTRUE;
                if (woken) {
                    const uint32_t shaved_ms =
                        (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - t0);
                    ESP_LOGI(TAG,
                             "Interview wake ieee=0x%016llx — retrying "
                             "now (shaved %lums)",
                             (unsigned long long)ev.ieee,
                             (unsigned long)(INTERVIEW_RETRY_DELAY_MS >
                                              shaved_ms
                                              ? INTERVIEW_RETRY_DELAY_MS -
                                                 shaved_ms
                                              : 0));
                }
                // Fairness: if another device joined while we were
                // sleeping, yield our slot so we don't starve them
                // through 10×30s of retry. Requeue current ev at the
                // tail; the outer loop picks the next peer first.
                if (uxQueueMessagesWaiting(s_join_queue) > 0) {
                    ESP_LOGI(TAG,
                             "Interview yield ieee=0x%016llx — peer "
                             "queued, will retry after",
                             (unsigned long long)ev.ieee);
                    // Force pool-nwk re-read on the next attempt.
                    ev.nwk = 0;
                    (void)xQueueSend(s_join_queue, &ev, 0);
                    break;
                }
            } else {
                ESP_LOGE(TAG, "Interview failed after %d attempts for ieee=0x%016llx",
                         INTERVIEW_MAX_ATTEMPTS, (unsigned long long)ev.ieee);
                // §4 (FINDINGS.md, :251 SMELL): a NODE_DESC failure makes
                // do_interview() return false BEFORE the work-copy's FAILED
                // state is ever committed, so the live slot stays at NONE
                // (set at the top of do_interview). The opportunistic
                // re-interview in zcl_refresh_liveness is keyed on FAILED —
                // with the slot stuck at NONE it never re-fires, and the
                // device is stranded until it rejoins. Stamp FAILED on the
                // live slot here, after the final attempt, so the next
                // inbound frame from a (sleepy) device triggers a retry.
                zigbee_pool_lock();
                if (ZapDevice* d = pool_find_by_ieee(ev.ieee)) {
                    if (d->interview_state !=
                            static_cast<uint8_t>(InterviewState::IDENTITY_READY)) {
                        d->interview_state =
                            static_cast<uint8_t>(InterviewState::FAILED);
                    }
                }
                zigbee_pool_unlock();
            }
        }
        s_active_interview_ieee.store(0, std::memory_order_release);
        s_active_interview_nwk .store(0xFFFE, std::memory_order_release);
    }
}

static void on_tc_dev_ind(const MtFrame& f) {
    if (f.payload_len < 11) {
        ESP_LOGW(TAG, "ZDO_TC_DEV_IND too short (%d)", f.payload_len);
        return;
    }
    JoinEvent ev{};
    ev.nwk  = le16(f.payload);
    ev.ieee = le64(f.payload + 2);
    // Propagate the fresh nwk into the pool immediately, even when an
    // earlier JoinEvent for this IEEE is still draining its retry
    // cycle — that loop re-reads the pool on each attempt.
    //
    // Critical: also seed an entry when the pool has none. Without this,
    // a rejoin that lands while no record exists (interview never
    // completed, or a previous reset cleared the entry) leaves
    // pool_find_by_nwk failing for every AF_INCOMING from the fresh
    // nwk, while task_interview later pops a stale-nwk JoinEvent from
    // the queue and re-creates the entry under the OLD nwk — symptom:
    // "AF_INCOMING from unknown nwk=…" loops on a live device.
    // Lock spans the full find-then-mutate sequence. Previously the
    // pool_find_by_ieee released the mutex on return, and a concurrent
    // pool_remove (swap-with-last from a user delete or ZDO_LEAVE_IND)
    // could relocate the entry at `d` between the lookup and the write
    // below — d->nwk_addr would then clobber a different device's record.
    // The mutex is recursive so the public mark_dirty / dev_clear_removed
    // calls below remain safe.
    bool seeded   = false;
    bool refreshed = false;
    uint16_t old_nwk = 0;
    bool cleared_removed = false;
    zigbee_pool_lock();
    ZapDevice* d = pool_find_by_ieee(ev.ieee);
    if (!d) {
        d = pool_add();
        if (d) {
            d->ieee_addr = ev.ieee;
            d->nwk_addr  = ev.nwk;
            d->interview_state = static_cast<uint8_t>(InterviewState::NONE);
            seeded = true;
        }
    } else if (d->nwk_addr != ev.nwk) {
        old_nwk = d->nwk_addr;
        d->nwk_addr = ev.nwk;
        refreshed = true;
    }
    if (d && zap_dev_is_removed(d)) {
        zap_dev_clear_removed(d);
        cleared_removed = true;
    }
    zigbee_pool_unlock();

    if (seeded || refreshed) zigbee_pool_mark_dirty();
    if (seeded) {
        ESP_LOGI(TAG, "Pool seeded on join ieee=0x%016llx nwk=0x%04x",
                 (unsigned long long)ev.ieee, ev.nwk);
    } else if (refreshed) {
        ESP_LOGI(TAG, "Rejoin nwk refresh ieee=0x%016llx %04x → %04x",
                 (unsigned long long)ev.ieee, old_nwk, ev.nwk);
    }
    if (cleared_removed) {
        ESP_LOGI(TAG, "Rejoin clears ZAP_DEV_REMOVED ieee=0x%016llx",
                 (unsigned long long)ev.ieee);
    }
    // Rejoin is itself a wake signal — kicks task_interview if it's
    // currently sleeping between attempts for this device.
    if (ev.ieee == s_active_interview_ieee.load(std::memory_order_acquire)) {
        interview_wake_notify();
    }
    ESP_LOGI(TAG, "Device joined nwk=0x%04x ieee=0x%016llx", ev.nwk, ev.ieee);
    // §4 (FINDINGS.md, :662 BLOCK): this handler runs in the ZNP UART RX
    // task. A blocking xQueueSend (was 100 ms) means a join storm that
    // fills s_join_queue stalls the RX loop for up to 100 ms PER announce —
    // long enough for the UART RX FIFO to overrun and corrupt in-flight
    // frames. Send with ZERO timeout and, on a full queue, drop the OLDEST
    // queued JoinEvent then retry: the pool was already seeded/refreshed
    // above and task_interview re-reads the pool nwk on each attempt, so
    // the freshest announce is the one worth keeping. The RX task never
    // blocks here.
    if (xQueueSend(s_join_queue, &ev, 0) != pdTRUE) {
        JoinEvent discarded;
        if (xQueueReceive(s_join_queue, &discarded, 0) == pdTRUE) {
            ESP_LOGW(TAG, "join queue full — evicted oldest (ieee=0x%016llx)",
                     (unsigned long long)discarded.ieee);
        }
        if (xQueueSend(s_join_queue, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "join queue full — drop newest nwk=0x%04x ieee=0x%016llx",
                     ev.nwk, (unsigned long long)ev.ieee);
        }
    }
    // Wake a currently-sleeping retry loop for a DIFFERENT device so it
    // yields its slot to this fresh join event. The retry loop itself
    // checks the queue and re-enqueues its current ev to the tail.
    const uint64_t active = s_active_interview_ieee.load(std::memory_order_acquire);
    if (ev.ieee != active && active != 0) {
        interview_wake_notify();
    }
}

// Drain any pending JoinEvent from s_join_queue. Called from zigbee_mgr
// during crash-recovery reinit so stale join events from before the crash
// don't re-trigger wrong-state interviews after the NCP comes back up
// (QWEN §6). Safe to call when s_join_queue isn't created yet.
void zigbee_interview_flush_join_queue() {
    if (!s_join_queue) return;
    JoinEvent tmp;
    size_t dropped = 0;
    while (xQueueReceive(s_join_queue, &tmp, 0) == pdTRUE) dropped++;
    if (dropped) ESP_LOGW(TAG, "reinit: flushed %u stale join events", (unsigned)dropped);
}

bool zigbee_interview_trigger(uint64_t ieee) {
    // F6/F35: short locked read — copy nwk out, never hold the raw
    // pool pointer past the unlock.
    uint16_t nwk = 0;
    bool found = false;
    zigbee_pool_lock();
    if (const ZapDevice* dev = pool_find_by_ieee(ieee)) {
        nwk = dev->nwk_addr;
        found = true;
    }
    zigbee_pool_unlock();
    if (!found) {
        ESP_LOGW(TAG, "interview_trigger: 0x%016llx not in pool", (unsigned long long)ieee);
        return false;
    }
    JoinEvent ev{}; ev.ieee = ieee; ev.nwk = nwk;
    if (xQueueSend(s_join_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "interview_trigger: queue full for 0x%016llx", (unsigned long long)ieee);
        return false;
    }
    ESP_LOGI(TAG, "interview_trigger: queued 0x%016llx nwk=0x%04x",
             (unsigned long long)ieee, nwk);
    return true;
}

void zigbee_interview_init() {
    s_join_queue = xQueueCreate(16, sizeof(JoinEvent));
    configASSERT(s_join_queue);
    s_rsp_sem = xSemaphoreCreateBinary();
    configASSERT(s_rsp_sem);
    s_basic_sem = xSemaphoreCreateBinary();
    configASSERT(s_basic_sem);
    s_wake_sem = xSemaphoreCreateBinary();
    configASSERT(s_wake_sem);

    xTaskCreate(task_interview, "zb_interview", zhac::stack::kZbInterview, nullptr, 5, nullptr);

    znp_register_areq(MT_AREQ(ZNP_ZDO), 0x82, store_rsp);    // NODE_DESC_RSP
    znp_register_areq(MT_AREQ(ZNP_ZDO), 0x85, store_rsp);    // ACTIVE_EP_RSP
    znp_register_areq(MT_AREQ(ZNP_ZDO), 0x84, store_rsp);    // SIMPLE_DESC_RSP
    znp_register_areq(MT_AREQ(ZNP_ZDO), 0xA1, store_rsp);    // BIND_RSP
    znp_register_areq(MT_AREQ(ZNP_ZDO), 0xCA, on_tc_dev_ind); // TC_DEV_IND

    ESP_LOGI(TAG, "zigbee_interview_init OK");
}
