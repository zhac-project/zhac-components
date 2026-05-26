// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_mgr/zigbee_mgr.cpp
#include "zigbee_mgr.h"
#include "znp_driver.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstring>
#include <ctime>
#include "zigbee_pool.h"
#include "zap_store.h"
#include "device_shadow.h"
#include "zhc_adapter.h"
#include "zigbee_identity.h"
#include "zigbee_configure_queue.h"
#include "zigbee_diagnostics.h"
#include "task_stacks.h"

static const char* TAG = "zigbee_mgr";

// zhc_adapter hook registrars — defined in zhc_send_bridge.cpp and
// zhc_shadow_bridge.cpp (same component). extern "C" matches the
// definitions' linkage.
extern "C" void zhc_send_bridge_register(void);
extern "C" void zhc_shadow_bridge_register(void);
extern "C" void zhc_configure_bridge_register(void);

// ── ZCL processing queue (Z14) ────────────────────────────────────────────
// Raw AF frames are copied here from the AREQ callback so the UART RX task
// is never blocked by converter/shadow work.
static constexpr uint8_t  ZCL_QUEUE_DEPTH   = 16;
static constexpr uint8_t  ZCL_PAYLOAD_MAX   = 128;

struct AfRawFrame {
    uint16_t group_id;     // AF_INCOMING_MSG bytes 0..1 (0 = unicast)
    uint16_t cluster_id;
    uint16_t src_nwk;
    uint8_t  src_ep;
    uint8_t  lqi;          // copied from AF_INCOMING_MSG byte 9
    uint8_t  data_len;
    uint8_t  data[ZCL_PAYLOAD_MAX];
};

static QueueHandle_t s_zcl_queue = nullptr;

// `zigbee_mgr_dispatch_command_rule` + `zigbee_mgr_translate_synthetic_attrs`
// removed 2026-04-19 with the rest of the legacy zcl_converter pipeline.
// ZHC decoders in `embedded/zhc/definitions/` now cover every device with
// dedicated FzConverters (action strings, IAS zone, cover commands, etc.).


static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Forward declarations (implemented in other .cpp files in this component)
void zigbee_interview_init();

// ── Coordinator IEEE (fetched once during init) ───────────────────────────
static uint64_t s_coordinator_ieee = 0;

uint64_t zigbee_mgr_coordinator_ieee() { return s_coordinator_ieee; }

// ── ZDO_STATE_CHANGE_IND wait ─────────────────────────────────────────────
// std::atomic instead of `volatile` (ZB-F4): the AREQ dispatch task
// writes, the commissioning / startup tasks busy-wait. Plain volatile
// is not a memory barrier on dual-core ESP32-P4 — the compiler is
// allowed to hoist the spin-wait read out of the loop on -O2. atomic
// gives us seq_cst defaults so reads see writes from the other core.
static std::atomic<bool> s_coordinator_ready{false};

static void on_state_change(const MtFrame& f) {
    if (f.payload_len < 1) return;
    uint8_t state = f.payload[0];
    ESP_LOGI(TAG, "ZDO_STATE_CHANGE_IND state=0x%02x", state);
    if (state == 0x09) {
        s_coordinator_ready = true;
    }
}

// ── SYS_RESET_IND: expected during startup, unexpected = ZNP crash ────────
// Same ZB-F4 reasoning as s_coordinator_ready — written from the AREQ
// dispatch task, polled from commissioning / startup paths.
static std::atomic<bool> s_reset_received{false};
static std::atomic<bool> s_znp_crashed{false};
static          bool     s_init_done = false;

static void on_reset_ind(const MtFrame&) {
    s_reset_received = true;
    if (s_init_done) {
        s_znp_crashed = true;
        ESP_LOGE(TAG, "Unexpected SYS_RESET_IND — ZNP crashed");
    } else {
        ESP_LOGI(TAG, "SYS_RESET_IND received");
    }
}

// ── Helper ────────────────────────────────────────────────────────────────
static bool sreq_expect_status_ok(uint8_t cmd0, uint8_t cmd1,
                                   const uint8_t* payload, uint8_t plen,
                                   const char* name) {
    MtFrame req{};
    req.cmd0 = cmd0; req.cmd1 = cmd1;
    req.payload = payload; req.payload_len = plen;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) {
        ESP_LOGE(TAG, "%s: no SRSP", name); return false;
    }
    if (rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
        ESP_LOGE(TAG, "%s: status=0x%02x", name,
                 rsp.payload_len ? rsp.payload[0] : 0xFF);
        return false;
    }
    return true;
}

// ── Z-Stack NV item IDs (from z-stack-3.x headers) ───────────────────────
static constexpr uint16_t NV_EXTADDR             = 0x0001;
static constexpr uint16_t NV_STARTUP_OPTION      = 0x0003;
static constexpr uint16_t NV_LOGICAL_TYPE        = 0x0087;
static constexpr uint16_t NV_ZDO_DIRECT_CB       = 0x008F;
static constexpr uint16_t NV_CHANLIST            = 0x0084;
static constexpr uint16_t NV_PANID               = 0x0083;
static constexpr uint16_t NV_EXTENDED_PAN_ID     = 0x002D;
static constexpr uint16_t NV_APS_USE_EXT_PANID   = 0x008E;
static constexpr uint16_t NV_PRECFGKEY           = 0x0062;
static constexpr uint16_t NV_PRECFGKEYS_ENABLE   = 0x0063;
static constexpr uint16_t NV_NIB                 = 0x0021;
static constexpr uint16_t NV_ZNP_HAS_CONFIGURED  = 0x0F00;

// ── NV item helpers (osalNvReadExt / osalNvWriteExt / osalNvItemInit) ────
// osalNvReadExt (SYS 0x1C): payload = id(2 LE) + offset(2 LE)
// Response = status(1) + len(1) + data[len]
static bool nv_read(uint16_t id, uint16_t offset, uint8_t* out, uint8_t* out_len,
                     uint8_t out_cap) {
    uint8_t pl[4] = {(uint8_t)(id & 0xFF), (uint8_t)(id >> 8),
                     (uint8_t)(offset & 0xFF), (uint8_t)(offset >> 8)};
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x1C;
    req.payload = pl; req.payload_len = sizeof(pl);
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return false;
    if (rsp.payload_len < 2 || rsp.payload[0] != 0x00) return false;
    uint8_t n = rsp.payload[1];
    if (n > out_cap) n = out_cap;
    if (out && n) memcpy(out, rsp.payload + 2, n);
    if (out_len) *out_len = n;
    return true;
}

// Forward decl — nv_write uses nv_item_init (defined below) to pre-create items on fresh NV.
static bool nv_item_init_fwd(uint16_t id, uint16_t item_len, const uint8_t* init, uint8_t init_len);

// osalNvWriteExt (SYS 0x1D): payload = id(2) + offset(2) + len(2) + data[len]
static bool nv_write_raw(uint16_t id, const uint8_t* data, uint16_t len) {
    uint8_t pl[6 + 128];
    if (len > 128) return false;
    pl[0] = id & 0xFF; pl[1] = id >> 8;
    pl[2] = 0; pl[3] = 0;
    pl[4] = len & 0xFF; pl[5] = len >> 8;
    if (len) memcpy(pl + 6, data, len);
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x1D;
    req.payload = pl; req.payload_len = 6 + len;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return false;
    return rsp.payload_len >= 1 && rsp.payload[0] == 0x00;
}

// Ensures item exists (via osalNvItemInit) then writes value.
// On fresh Z-Stack NV, items must be initialized before osalNvWriteExt.
static bool nv_write(uint16_t id, const uint8_t* data, uint16_t len) {
    // Pre-create with same length+init value so first-boot works.
    uint8_t init_len = len > 128 ? 0 : (uint8_t)len;
    if (!nv_item_init_fwd(id, len, data, init_len)) {
        // non-fatal: item may already exist — proceed to write
    }
    if (nv_write_raw(id, data, len)) return true;
    ESP_LOGE(TAG, "NV write id=0x%04x len=%u failed after init", id, len);
    return false;
}

// osalNvItemInit (SYS 0x07): payload = id(2) + itemlen(2) + initlen(1) + init[initlen]
static bool nv_item_init_fwd(uint16_t id, uint16_t item_len,
                              const uint8_t* init, uint8_t init_len);
static bool nv_item_init(uint16_t id, uint16_t item_len,
                          const uint8_t* init, uint8_t init_len) {
    return nv_item_init_fwd(id, item_len, init, init_len);
}
static bool nv_item_init_fwd(uint16_t id, uint16_t item_len,
                              const uint8_t* init, uint8_t init_len) {
    uint8_t pl[5 + 128];
    if (init_len > 128) return false;
    pl[0] = id & 0xFF; pl[1] = id >> 8;
    pl[2] = item_len & 0xFF; pl[3] = item_len >> 8;
    pl[4] = init_len;
    if (init_len) memcpy(pl + 5, init, init_len);
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x07;
    req.payload = pl; req.payload_len = 5 + init_len;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return false;
    // Status: 0x00=success, 0x09=already exists — both fine for our use
    if (rsp.payload_len < 1) return false;
    uint8_t st = rsp.payload[0];
    return st == 0x00 || st == 0x09;
}

// osalNvLength (SYS 0x13): payload = id(2). Returns length(2).
static uint16_t nv_length(uint16_t id) {
    uint8_t pl[2] = {(uint8_t)(id & 0xFF), (uint8_t)(id >> 8)};
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x13;
    req.payload = pl; req.payload_len = 2;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return 0;
    if (rsp.payload_len < 2) return 0;
    return (uint16_t)rsp.payload[0] | ((uint16_t)rsp.payload[1] << 8);
}

// osalNvDelete (SYS 0x12): payload = id(2) + len(2). Returns status(1).
static bool nv_delete(uint16_t id, uint16_t len) {
    uint8_t pl[4] = {(uint8_t)(id & 0xFF), (uint8_t)(id >> 8),
                     (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x12;
    req.payload = pl; req.payload_len = 4;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return false;
    // 0x00=success, 0x09=item not found — both OK
    return rsp.payload_len >= 1 && (rsp.payload[0] == 0x00 || rsp.payload[0] == 0x09);
}

// Phase 2 strategy probe — logs NV presence; z-stack 2.7 uses these to pick strategy.
static void log_strategy_probe() {
    ESP_LOGI(TAG, "Strategy probe: NIB=%u PRECFGKEY=%u NWK_ACTIVE=%u NWK_ALTERN=%u",
             nv_length(NV_NIB), nv_length(0x0062 /*PRECFGKEY*/),
             nv_length(0x003A /*NWK_ACTIVE_KEY_INFO*/),
             nv_length(0x003B /*NWK_ALTERN_KEY_INFO*/));
}

// ── SYS_VERSION (0x02): {transportrev, product, majorrel, minorrel, maintrel, revision}
static bool sys_version(uint8_t* product_out) {
    MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x02;
    MtFrame rsp{};
    if (!znp_sreq_retry(req, rsp, 3000, 3)) return false;
    if (rsp.payload_len < 6) return false;
    if (product_out) *product_out = rsp.payload[1];
    ESP_LOGI(TAG, "CC2652 version: transportrev=%u product=%u rel=%u.%u.%u rev=0x%02x%02x%02x%02x",
             rsp.payload[0], rsp.payload[1], rsp.payload[2], rsp.payload[3], rsp.payload[4],
             rsp.payload_len >= 10 ? rsp.payload[9] : 0,
             rsp.payload_len >= 9  ? rsp.payload[8] : 0,
             rsp.payload_len >= 8  ? rsp.payload[7] : 0,
             rsp.payload_len >= 7  ? rsp.payload[6] : 0);
    return true;
}

// ── Strategy: check NIB presence ─────────────────────────────────────────
// Returns true if stick is already configured (skip commissioning).
static bool is_configured() {
    // NIB is set by the ZNP itself during BDB network formation; its
    // presence is the authoritative signal that a network exists.
    // The HAS_CONFIGURED marker (osalNvItemInit-managed) was unreliable
    // — if the item ever existed with the wrong size, init became a
    // no-op and the marker stayed stale across reboots, even though the
    // network state was intact.
    uint16_t nib_len = nv_length(NV_NIB);
    if (nib_len > 0) {
        ESP_LOGI(TAG, "NIB present (%u bytes) — already configured", nib_len);
        return true;
    }
    ESP_LOGI(TAG, "NIB absent — needs commissioning");
    return false;
}

// ── Commissioning (Phase 3a per zigbee-herdsman COORDINATOR.md) ───────────
// Writes commissioning NV items, starts BDB formation, waits for state=9.
static bool do_commissioning() {
    ESP_LOGI(TAG, "Starting BDB commissioning (first boot / factory reset)");
    log_strategy_probe();

    // Phase 3a step 1: osalNvDelete NIB (clear old network state)
    {
        uint16_t nib_len = nv_length(NV_NIB);
        if (nib_len > 0) {
            ESP_LOGI(TAG, "Deleting old NIB (%u bytes)", nib_len);
            nv_delete(NV_NIB, nib_len);
        }
    }

    // STARTUP_OPTION = 0x03 (clear network state + clear config) — apply via soft reset
    { uint8_t v = 0x03; if (!nv_write(NV_STARTUP_OPTION, &v, 1))
        { ESP_LOGE(TAG, "NV STARTUP_OPTION=0x03 failed"); return false; } }

    // SYS_RESET soft — apply STARTUP_OPTION=0x03 by resetting
    {
        s_reset_received = false;
        uint8_t pl = 0x01;
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x00;
        req.payload = &pl; req.payload_len = 1;
        MtFrame rsp{};
        znp_sreq_retry(req, rsp, 3000, 1);  // SYS_RESET_REQ has no SRSP, just AREQ
        for (int i = 0; i < 50 && !s_reset_received; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    // STARTUP_OPTION = 0x00
    { uint8_t v = 0x00; if (!nv_write(NV_STARTUP_OPTION, &v, 1)) return false; }

    // ── Pull runtime Zigbee settings from NVS (namespace `zigbee_cfg`).
    // These control the on-air identity of the network; flipping either
    // one forces a re-pair of every device. Defaults:
    //   channel = 11 (z2m default; user can move via /api/zigbee/settings)
    //   net_key = generated by esp_fill_random on first commissioning
    //             and persisted so subsequent re-commissions use the
    //             same key unless the operator explicitly overrides.
    uint8_t chan = 11;
    uint8_t net_key[16] = {};
    bool    net_key_present = false;
    {
        nvs_handle_t h;
        if (nvs_open("zigbee_cfg", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "channel", &chan);
            size_t keylen = sizeof(net_key);
            if (nvs_get_blob(h, "net_key", net_key, &keylen) == ESP_OK
                && keylen == 16) {
                net_key_present = true;
            }
            nvs_close(h);
        }
    }
    if (chan < 11 || chan > 26) chan = 11;
    if (!net_key_present) {
        esp_fill_random(net_key, sizeof(net_key));
        nvs_handle_t h;
        if (nvs_open("zigbee_cfg", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "net_key", net_key, sizeof(net_key));
            nvs_set_u8  (h, "channel", chan);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "Generated new random Zigbee network key "
                      "(persisted to NVS zigbee_cfg::net_key)");
    }
    const uint32_t chan_mask = (uint32_t)1u << chan;
    ESP_LOGI(TAG, "Commissioning on channel %u (mask=0x%08lx), "
                  "net_key %s",
             (unsigned)chan, (unsigned long)chan_mask,
             net_key_present ? "loaded from NVS" : "freshly generated");

    // Config writes — ORDER MATTERS (matches working PHP coordinator flow):
    // LOGICAL_TYPE = 0x00 (COORDINATOR)
    { uint8_t v = 0x00; if (!nv_write(NV_LOGICAL_TYPE, &v, 1)) return false; }
    // PRECFGKEYS_ENABLE = 1 (enable BEFORE writing key — devices need TC link key)
    { uint8_t v = 0x01; if (!nv_write(NV_PRECFGKEYS_ENABLE, &v, 1)) return false; }
    // ZDO_DIRECT_CB = 0x01 (enable AREQ callbacks for join/leave)
    { uint8_t v = 0x01; if (!nv_write(NV_ZDO_DIRECT_CB, &v, 1)) return false; }
    // PANID = 0xFFFF (auto-pick)
    { uint8_t v[2] = {0xFF, 0xFF}; if (!nv_write(NV_PANID, v, 2)) return false; }
    // CHANLIST — single channel from NVS.
    {
        uint8_t v[4] = {
            (uint8_t)(chan_mask      & 0xFF),
            (uint8_t)((chan_mask>> 8) & 0xFF),
            (uint8_t)((chan_mask>>16) & 0xFF),
            (uint8_t)((chan_mask>>24) & 0xFF),
        };
        if (!nv_write(NV_CHANLIST, v, 4)) return false;
    }
    // PRECFGKEY — network key from NVS (random on first commission).
    if (!nv_write(NV_PRECFGKEY, net_key, 16)) return false;
    // EXTENDED_PAN_ID + APS_USE_EXT_PANID = FF*8 (auto-pick)
    { uint8_t v[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      if (!nv_write(NV_EXTENDED_PAN_ID, v, 8)) return false;
      if (!nv_write(NV_APS_USE_EXT_PANID, v, 8)) return false; }

    // BDB set primary channel (single channel from NVS).
    {
        uint8_t v[5] = {
            0x01,
            (uint8_t)(chan_mask      & 0xFF),
            (uint8_t)((chan_mask>> 8) & 0xFF),
            (uint8_t)((chan_mask>>16) & 0xFF),
            (uint8_t)((chan_mask>>24) & 0xFF),
        };
        if (!sreq_expect_status_ok(MT_SREQ(ZNP_APP_CNF), 0x08, v, 5, "BDB_SET_CHANNEL_PRIMARY"))
            return false;
    }
    // BDB set secondary channel = 0 (none)
    { uint8_t v[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
      if (!sreq_expect_status_ok(MT_SREQ(ZNP_APP_CNF), 0x08, v, 5, "BDB_SET_CHANNEL_SECONDARY"))
          return false; }

    // BDB start commissioning mode 0x04 (network formation).
    // SRSP may time out on some Z-Stack builds — BDB task holds the SREQ
    // during formation (seconds). Rely on state=9 AREQ to confirm success.
    s_coordinator_ready = false;
    { uint8_t v = 0x04;
      MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_APP_CNF); req.cmd1 = 0x05;
      req.payload = &v; req.payload_len = 1;
      MtFrame rsp{};
      znp_sreq_retry(req, rsp, 3000, 1);  // best-effort — ignore SRSP outcome
      ESP_LOGI(TAG, "BDB commissioning mode=0x04 sent — waiting state=9");
    }

    // Wait state=9 (COORDINATOR_STARTED) — BDB formation takes several seconds
    for (int i = 0; i < 200 && !s_coordinator_ready; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_coordinator_ready) {
        ESP_LOGE(TAG, "BDB commissioning: no state=9 within 20 s"); return false;
    }
    ESP_LOGI(TAG, "BDB network formed");

    // Phase 3a step 7+8: poll ZDO_EXT_NWK_INFO (0x50) until panId != 0xFFFF.
    // Authoritative source for network state — works across Z-Stack 2.x/3.x.
    {
        uint16_t panid = 0xFFFF;
        uint16_t shortaddr = 0xFFFF;
        uint8_t  devstate = 0;
        for (int i = 0; i < 50; i++) {
            MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x50;
            MtFrame rsp{};
            if (znp_sreq_retry(req, rsp, 3000, 1) && rsp.payload_len >= 7) {
                shortaddr = rsp.payload[0] | (rsp.payload[1] << 8);
                devstate  = rsp.payload[2];
                panid     = rsp.payload[3] | (rsp.payload[4] << 8);
                if (panid != 0xFFFF && panid != 0x0000) break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (panid == 0xFFFF) {
            ESP_LOGE(TAG, "Network did not form (panId stuck at 0xFFFF)");
            return false;
        }
        ESP_LOGI(TAG, "Network up: shortaddr=0x%04X devstate=%u panid=0x%04X",
                 shortaddr, devstate, panid);
    }

    // BDB NETWORK_STEERING (mode 0x02) — opens network for joining devices.
    // Without this, NETWORK_FORMATION completes but no devices can join even
    // with permit_join open. PHP/z2m both issue this right after formation.
    {
        uint8_t v = 0x02;
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_APP_CNF); req.cmd1 = 0x05;
        req.payload = &v; req.payload_len = 1;
        MtFrame rsp{};
        znp_sreq_retry(req, rsp, 3000, 1);  // best-effort
        ESP_LOGI(TAG, "BDB network steering (mode=0x02) sent");
    }

    // Final ZDO_STARTUP_FROM_APP — brings coordinator to fully-ready state after BDB.
    {
        uint8_t pl[2] = {0x00, 0x00};
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x40;
        req.payload = pl; req.payload_len = 2;
        MtFrame rsp{};
        znp_sreq_retry(req, rsp, 3000, 3);
    }

    // Write ZNP_HAS_CONFIGURED marker (0x55) so next boot skips commissioning
    { uint8_t init = 0x55;
      if (!nv_item_init(NV_ZNP_HAS_CONFIGURED, 1, &init, 1))
          ESP_LOGW(TAG, "nv_item_init(HAS_CONFIGURED) failed (non-fatal)"); }

    return true;
}

// AF_INCOMING_MSG AREQ: cmd0=MT_AREQ(ZNP_AF), cmd1=0xFF
// Payload layout: group_id(2) cluster_id(2) src_addr(2) src_ep(1) dst_ep(1)
//                 was_broadcast(1) lqi(1) security(1) timestamp(4) trans_seq(1)
//                 len(1) data[len]
// Callback runs in UART RX task — only copy frame, never block.
// Forward declaration — defined in zigbee_interview.cpp
extern void zigbee_interview_on_af_incoming(const uint8_t* payload, uint8_t payload_len);

static void on_af_incoming_msg(const MtFrame& f) {
    if (f.payload_len < 17) return;

    // Let the interview interceptor check for Basic cluster responses
    zigbee_interview_on_af_incoming(f.payload, f.payload_len);

    // Late-identity enrichment: any Basic-cluster frame (report or
    // read-attr response) promotes IDENTITY_PENDING devices and triggers
    // re-match. Non-Basic frames are filtered cheaply inside the helper.
    zigbee_identity_on_af_incoming(f.payload, f.payload_len);

    uint8_t data_len = f.payload[16];
    if (17u + data_len > f.payload_len) return;

    AfRawFrame frame{};
    frame.group_id   = static_cast<uint16_t>(f.payload[0]) | (static_cast<uint16_t>(f.payload[1]) << 8);
    frame.cluster_id = static_cast<uint16_t>(f.payload[2]) | (static_cast<uint16_t>(f.payload[3]) << 8);
    frame.src_nwk    = static_cast<uint16_t>(f.payload[4]) | (static_cast<uint16_t>(f.payload[5]) << 8);
    frame.src_ep     = f.payload[6];
    frame.lqi        = f.payload[9];   // AF_INCOMING_MSG layout: lqi at byte 9
    frame.data_len   = (data_len < ZCL_PAYLOAD_MAX) ? data_len : ZCL_PAYLOAD_MAX;
    memcpy(frame.data, f.payload + 17, frame.data_len);

    if (xQueueSend(s_zcl_queue, &frame, 0) != pdTRUE) {
        // Drop oldest, not newest. On a burst the most recent values are
        // the ones we actually want to deliver; letting the queue run stale
        // while dropping fresh data was the old behavior (QWEN §4/CODEX §4
        // — which also noted the README already promised drop-oldest).
        AfRawFrame discarded;
        if (xQueueReceive(s_zcl_queue, &discarded, 0) == pdTRUE) {
            ESP_LOGW(TAG, "ZCL queue full — evicted oldest");
        }
        if (xQueueSend(s_zcl_queue, &frame, 0) != pdTRUE) {
            ESP_LOGW(TAG, "ZCL queue full — drop newest after evict failed");
        }
    }
}

// Processes AF frames from s_zcl_queue — runs in its own task.
// Refresh liveness counters on every inbound frame. Returns the IEEE
// of a device whose interview left FAILED so the caller can kick a
// retry (or 0 when no retry is needed). Kept in memory only — NVS
// persistence happens lazily through existing paths (interview save,
// shadow NVS flush); writing per-frame would burn flash.
static uint64_t zcl_refresh_liveness(ZapDevice* dev, const AfRawFrame& frame) {
    zigbee_pool_lock();
    // last_seen is consumed by the SPA as a Unix epoch — only write
    // when SNTP has plausibly synced (post 2020-01-01) to avoid
    // shipping 1970+small_uptime values that render as "55 years ago".
    const time_t now = time(nullptr);
    if (now > 1577836800) {
        dev->last_seen = (uint32_t)now;
    }
    dev->link_quality = frame.lqi;
    // Opportunistic re-interview: sleepy end-devices (Aqara buttons,
    // door sensors) often miss the wake window during the initial
    // interview and end up as interview_state=FAILED. Every inbound
    // frame is proof they're awake — flip state to NONE first so
    // subsequent frames during the retry window don't re-enqueue.
    const bool retry_interview = (dev->interview_state ==
        static_cast<uint8_t>(InterviewState::FAILED));
    if (retry_interview) {
        dev->interview_state = static_cast<uint8_t>(InterviewState::NONE);
    }
    const uint64_t retry_ieee = retry_interview ? dev->ieee_addr : 0;
    zigbee_pool_unlock();
    return retry_ieee;
}

// Tuya end-devices (MiBoxer FUT089Z, many TS0601 remotes) poll
// cluster 0x000A attr 0x0007 (LocalTime) at join and gate button
// reporting on the answer. Returns true when a read was claimed —
// caller must skip further processing to keep ownership single-source.
static bool zcl_maybe_respond_gen_time(const AfRawFrame& frame,
                                        const uint8_t* zcl, uint8_t data_len) {
    if (frame.cluster_id != 0x000A || data_len < 3) return false;
    const uint8_t fc_byte   = zcl[0];
    const bool    profile   = (fc_byte & 0x03) == 0x00;
    const bool    manu_spec = (fc_byte & 0x04) != 0;
    const size_t  zcl_hdr   = manu_spec ? 5u : 3u;
    if (!(profile && !manu_spec && data_len >= zcl_hdr &&
          zcl[2] == 0x00 /* CMD_READ_ATTR */)) return false;
    const uint8_t  tsn      = zcl[1];
    const uint8_t* req_body = zcl + zcl_hdr;
    const size_t   req_len  = data_len - zcl_hdr;
    zigbee_respond_gen_time(frame.src_nwk, frame.src_ep, tsn, req_body, req_len);
    return true;
}

// IAS Zone (cluster 0x0500) auto-enroll. Sleepy IAS sensors (water
// leak, smoke, motion, contact) sit "Not Enrolled" until the
// coordinator (a) writes attr 0x0010 IAS_CIE_Address and (b)
// answers ZoneEnrollRequest (cmd 0x01, S→C) with EnrollResponse
// (cmd 0x00, C→S, [code=0x00 success, zone_id=0x00]). z2m bakes
// the same handshake into its `iasWarning`/`iasZoneAlarm` extends.
//
// Returns true when the frame was claimed; caller skips further
// dispatch (the device only sends EnrollRequest while not enrolled —
// once enrolled it ships ZoneStatusChangeNotifications instead).
static bool zcl_maybe_respond_ias_enroll(const AfRawFrame& frame,
                                          const uint8_t* zcl,
                                          uint8_t data_len) {
    if (frame.cluster_id != 0x0500 || data_len < 3) return false;
    const uint8_t fc_byte = zcl[0];
    const bool    cs      = (fc_byte & 0x03) == 0x01;
    const bool    mfg     = (fc_byte & 0x04) != 0;
    const std::size_t hdr = mfg ? 5u : 3u;
    if (data_len < hdr) return false;
    const uint8_t cmd_id = mfg ? zcl[4] : zcl[2];
    if (!cs || cmd_id != 0x01) return false;   // not a ZoneEnrollRequest

    const uint64_t coord = zigbee_mgr_coordinator_ieee();
    if (coord != 0) {
        uint8_t cie_le[8];
        for (uint8_t i = 0; i < 8; ++i) {
            cie_le[i] = static_cast<uint8_t>((coord >> (i * 8)) & 0xFF);
        }
        // Attr 0x0010 IAS_CIE_Address, type 0xF0 (IEEE Address). Many
        // sensors don't strictly require it post-EnrollResponse but a
        // few (Develco, Heiman) refuse to enroll until it's present.
        // Idempotent — safe to write on every request.
        zigbee_zcl_write_attr(frame.src_nwk, frame.src_ep, 0x0500,
                               0x0010, 0xF0, cie_le, sizeof(cie_le));
    }
    // ZoneEnrollResponse: cmd 0x00, payload [enroll_code=0x00 success,
    // zone_id=0x00]. Direction C→S, cluster-specific (FC bit 0 = 1).
    const uint8_t resp[] = { 0x00, 0x00 };
    zigbee_zcl_cluster_command(frame.src_nwk, frame.src_ep, 0x0500,
                                0x00, resp, sizeof(resp), 0);
    ESP_LOGI(TAG, "ias_zone enroll: nwk=0x%04x ep=%u responded",
              frame.src_nwk, frame.src_ep);
    return true;
}

// ZCL Foundation global (profile-wide) commands that must NOT be answered
// with a Default Response. A DR acknowledges a *command*; you never ACK a
// frame that is itself a response, nor one that explicitly opted out:
//   0x01 Read Attributes Response          0x0D Discover Attributes Response
//   0x04 Write Attributes Response         0x10 Write Attributes Structured Rsp
//   0x05 Write Attributes No Response      0x12 Discover Commands Received Rsp
//   0x07 Configure Reporting Response      0x14 Discover Commands Generated Rsp
//   0x09 Read Reporting Configuration Rsp  0x16 Discover Attributes Extended Rsp
//   0x0B Default Response (also caught upstream as a loop guard)
// Everything else — reads, writes, configure-reporting, discover, and the
// unsolicited Report Attributes (0x0A) — stays DR-eligible so sleepy
// reporters still get their ZCL-level ACK. Cluster-specific frames are
// always genuine commands and are never gated by this list.
static bool zcl_global_cmd_wants_default_response(uint8_t cmd_id) {
    switch (cmd_id) {
        case 0x01: case 0x04: case 0x05: case 0x07: case 0x09:
        case 0x0B: case 0x0D: case 0x10: case 0x12: case 0x14: case 0x16:
            return false;
        default:
            return true;
    }
}

// Tuya sleepy end-devices retransmit up to 5× when the outgoing frame
// control has bit 4 ("disable default response") clear and we don't
// ACK. Respond to every unicast frame except Default Responses
// themselves (loop prevention) and frames that opted out of the ACK.
static void zcl_send_default_response_if_needed(const AfRawFrame& frame,
                                                 const uint8_t* zcl,
                                                 uint8_t data_len) {
    if (frame.group_id != 0 || data_len < 3) return;
    const uint8_t in_fc   = zcl[0];
    const bool    mfg     = (in_fc & 0x04) != 0;
    const size_t  hdr_len = mfg ? 5u : 3u;
    // Gate header reads on full header presence — a truncated mfg-spec
    // frame (data_len < 5) would otherwise read zcl[3..4] past the end
    // of the received buffer.
    if (data_len < hdr_len) return;
    const bool     is_cs    = (in_fc & 0x03) == 0x01;
    const uint8_t  tsn      = mfg ? zcl[3] : zcl[1];
    const uint8_t  cmd_id   = mfg ? zcl[4] : zcl[2];
    const uint16_t mfg_code = mfg
        ? (static_cast<uint16_t>(zcl[1]) |
           (static_cast<uint16_t>(zcl[2]) << 8))
        : 0;
    const bool is_default_response = !is_cs && (cmd_id == 0x0B);
    const bool disable_default     = (in_fc & 0x10) != 0;
    if (is_default_response || disable_default) return;
    // ZCL spec: a Default Response ACKs a command, never another response.
    // Cluster-specific frames are always commands; among global frames,
    // skip the ones that are themselves responses (or opted out of ACK).
    if (!is_cs && !zcl_global_cmd_wants_default_response(cmd_id)) return;
    zigbee_send_default_response(frame.src_nwk, frame.src_ep,
                                  frame.cluster_id, in_fc,
                                  mfg_code, tsn, cmd_id, 0x00);
}

// ZHC missed the frame — record it for the Diagnostics UI and publish
// a ZCL_RAW event so the rules engine can still match on raw frames.
static void zcl_publish_raw_fallback(const AfRawFrame& frame,
                                      const ZapDevice* dev,
                                      const uint8_t* zcl, uint8_t data_len) {
    if (data_len < 3) return;
    const uint8_t fc  = zcl[0];
    const uint8_t cmd = zcl[2];
    const bool cs = (fc & 0x03) == 0x01;
    uint16_t ac = cmd;
    if (!cs && data_len >= 5) {
        ac = static_cast<uint16_t>(zcl[3]) |
             (static_cast<uint16_t>(zcl[4]) << 8);
    }
    zb_diag_record_unhandled(frame.cluster_id, ac, cs,
                              dev ? dev->ieee_addr : 0);

    Event raw_ev{};
    raw_ev.type = EventType::ZCL_RAW;
    auto* rp = reinterpret_cast<ZclRawEvent*>(raw_ev.data);
    rp->ieee    = dev ? dev->ieee_addr : 0;
    rp->nwk     = frame.src_nwk;
    rp->ep      = frame.src_ep;
    rp->cluster = frame.cluster_id;
    rp->command = cs ? (uint8_t)(cmd | 0x80) : (uint8_t)cmd;
    rp->payload_len = (uint8_t)(data_len < 40 ? data_len : 40);
    for (uint8_t bi = 0; bi < rp->payload_len; bi++) {
        snprintf(rp->payload_hex + bi * 2, 3, "%02x", zcl[bi]);
    }
    event_bus_publish(raw_ev);
}

// TaskZclAttr body — pipeline of per-concern helpers above.
static void zcl_attr_task(void*) {
    AfRawFrame frame{};
    for (;;) {
        if (xQueueReceive(s_zcl_queue, &frame, portMAX_DELAY) != pdTRUE) continue;

        const uint8_t* zcl      = frame.data;
        uint8_t        data_len = frame.data_len;

        ZapDevice* dev = pool_find_by_nwk(frame.src_nwk);
        if (!dev) {
            ESP_LOGW(TAG, "AF_INCOMING from unknown nwk=0x%04x cluster=0x%04x "
                          "(dropping; try re-interview)",
                     frame.src_nwk, frame.cluster_id);
            continue;
        }
        ESP_LOGD(TAG, "zcl_attr rx nwk=0x%04x cluster=0x%04x ep=%u grp=0x%04x "
                      "len=%u ieee=0x%016llx model='%s'",
                 frame.src_nwk, frame.cluster_id, frame.src_ep, frame.group_id,
                 (unsigned)data_len, (unsigned long long)dev->ieee_addr,
                 dev->model_id);

        const uint64_t retry_ieee = zcl_refresh_liveness(dev, frame);
        if (retry_ieee) {
            ESP_LOGI(TAG, "opportunistic re-interview ieee=0x%016llx "
                          "(FAILED → retrying now)",
                     (unsigned long long)retry_ieee);
            zigbee_interview_trigger(retry_ieee);
        }

        if (zcl_maybe_respond_gen_time(frame, zcl, data_len)) continue;

        if (zcl_maybe_respond_ias_enroll(frame, zcl, data_len)) continue;

        zcl_send_default_response_if_needed(frame, zcl, data_len);

        // ZHC library FIRST — 373-vendor / 4000+ device catalogue.
        // Hand the (ieee, nwk) tuple to the adapter so fz handlers
        // that emit a response (Zosung IR, Tuya MCU sync time) reach
        // the device with a valid destination.
        zhac_adapter_set_runtime_addr(dev->ieee_addr, dev->nwk_addr);
        if (zhac_adapter_try_decode(dev->ieee_addr,
                                     dev->model_id, dev->manufacturer_name,
                                     frame.group_id, frame.cluster_id,
                                     frame.src_ep, frame.lqi,
                                     zcl, data_len)) continue;

        zcl_publish_raw_fallback(frame, dev, zcl, data_len);
    }
}

// ZDO_LEAVE_IND AREQ (cmd0=MT_AREQ(ZNP_ZDO), cmd1=0xC9)
// Payload: nwk(2 LE) + ieee(8 LE) + remove?(1) + rejoin?(1)
// Device is kept in NVS/pool (retains friendly name for when it rejoins).
static void on_zdo_leave_ind(const MtFrame& f) {
    if (f.payload_len < 10) {
        ESP_LOGW(TAG, "ZDO_LEAVE_IND too short (%d)", f.payload_len);
        return;
    }
    uint64_t ieee = le64(f.payload + 2);
    ESP_LOGI(TAG, "Device left ieee=0x%016llX (soft-remove)",
             (unsigned long long)ieee);

    // Soft-remove: flip ZAP_DEV_REMOVED on the pool entry and persist
    // the single-byte change. The record stays in pool + NVS so the
    // friendly name, interview state, and shadow cache survive until
    // the device either rejoins (flag cleared by on_tc_dev_ind) or the
    // user hard-deletes via `zigbee_pool_remove`.
    if (ZapDevice* d = pool_find_by_ieee(ieee)) {
        zap_dev_mark_removed(d);
        zap_store_mark_dirty(d, ZAP_PERSIST_LOW);
    }

    Event ev{};
    ev.type = EventType::DEVICE_LEAVE;
    memcpy(ev.data, &ieee, sizeof(ieee));
    event_bus_publish(ev);
}

// ── Coordinator startup sequence (steps 1-9, called by init and reinit) ──
static bool coordinator_start() {
    // Clear all startup flags; suppress crash detection while we trigger a reset
    s_init_done         = false;
    s_reset_received    = false;
    s_coordinator_ready = false;
    s_znp_crashed       = false;

    // Step 1-2: hardware reset, wait SYS_RESET_IND.
    //
    // Retry the reset-pin toggle a few times before giving up — on cold
    // boot the ZNP occasionally misses the first SYS_RESET_IND window
    // (UART sync race, lingering bootloader, TX buffer stale). Each
    // attempt re-asserts the reset pin so we can recover without
    // rebooting P4 (which would drop the device pool, shadow cache and
    // running tasks).
    constexpr int kResetAttempts    = 5;
    constexpr int kResetWaitPer100Ms = 60;   // 6 s per attempt
    for (int attempt = 1; attempt <= kResetAttempts; ++attempt) {
        s_reset_received = false;
        if (attempt > 1) {
            ESP_LOGW(TAG, "SYS_RESET_IND retry %d/%d — re-asserting NRESET",
                      attempt, kResetAttempts);
            // Brief settle before re-toggling the pin; some C6/CC2652
            // bootloaders drop commands while the reset line was last
            // toggled a few ms ago.
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        znp_hw_reset();
        for (int i = 0; i < kResetWaitPer100Ms && !s_reset_received; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (s_reset_received) break;
    }
    if (!s_reset_received) {
        ESP_LOGE(TAG, "SYS_RESET_IND timeout after %d hw-reset attempts",
                  kResetAttempts);
        return false;
    }

    // Step 3: SYS_PING
    {
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x01;
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 3000, 3)) {
            ESP_LOGE(TAG, "SYS_PING failed"); return false;
        }
        ESP_LOGI(TAG, "SYS_PING OK — CC2652 alive");
    }

    // Step 3b: SYS_GET_EXTADDR — fetch coordinator's own IEEE address
    {
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_SYS); req.cmd1 = 0x04;
        MtFrame rsp{};
        if (znp_sreq_retry(req, rsp, 3000, 3) && rsp.payload_len >= 8) {
            uint64_t ieee = 0;
            for (int i = 0; i < 8; i++) ieee |= (uint64_t)rsp.payload[i] << (8 * i);
            s_coordinator_ieee = ieee;
            ESP_LOGI(TAG, "Coordinator IEEE = 0x%016llX", (unsigned long long)ieee);
        } else {
            ESP_LOGW(TAG, "SYS_GET_EXTADDR failed — bind will use IEEE=0");
        }
    }

    // Step 3c: SYS_VERSION (informational; drives product detection)
    { uint8_t product = 0; sys_version(&product); }

    // Step 4: determine startup strategy (commission vs normal-start)
    bool freshly_commissioned = false;
    if (!is_configured()) {
        // Phase 3a: Commissioning — writes all NV items + BDB formation.
        // s_coordinator_ready gets set true by state=9 AREQ inside do_commissioning.
        if (!do_commissioning()) {
            ESP_LOGE(TAG, "Commissioning failed"); return false;
        }
        ESP_LOGI(TAG, "Commissioning complete");
        freshly_commissioned = true;
    }

    // Step 5 + 6: ZDO_STARTUP_FROM_APP + wait state=9 — only for Phase 3c
    // (already-configured stick). Freshly commissioned sticks are already in state=9.
    if (!freshly_commissioned) {
        s_coordinator_ready = false;
    {
        uint8_t pl[] = {0x00, 0x00};
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x40;
        req.payload = pl; req.payload_len = sizeof(pl);
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 3000, 3)) {
            ESP_LOGE(TAG, "ZDO_STARTUP_FROM_APP: no SRSP"); return false;
        }
        ESP_LOGI(TAG, "ZDO_STARTUP_FROM_APP sent — waiting coordinator...");
    }

        // Step 6: wait ZDO_STATE_CHANGE_IND state=9
        for (int i = 0; i < 100 && !s_coordinator_ready; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_coordinator_ready) {
            ESP_LOGE(TAG, "Coordinator state timeout"); return false;
        }
        ESP_LOGI(TAG, "Coordinator formed");
    }  // end if (!freshly_commissioned)

    // Step 7: AF_REGISTER (EP=1, profile=0x0104 HA, device=0x0005)
    {
        uint8_t pl[] = {
            0x01,         // endpoint = 1
            0x04, 0x01,   // app_profile_id = 0x0104 HA
            0x05, 0x00,   // app_device_id = 0x0005
            0x00,         // app_device_version
            0x00,         // latency
            0x00,         // app_in_cluster_count = 0
            0x00,         // app_out_cluster_count = 0
        };
        // AF_REGISTER: status 0x00=success, 0xB8=ZApsDuplicateEntry, 0x08=ZMemError/already-exists — all OK.
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x00;
        req.payload = pl; req.payload_len = sizeof(pl);
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 3000, 3)) {
            ESP_LOGE(TAG, "AF_REGISTER: no SRSP"); return false;
        }
        uint8_t st = rsp.payload_len ? rsp.payload[0] : 0xFF;
        if (st != 0x00 && st != 0x08 && st != 0xB8) {
            ESP_LOGE(TAG, "AF_REGISTER: status=0x%02x", st); return false;
        }
        ESP_LOGI(TAG, "AF_REGISTER EP1 %s (status=0x%02x)",
                 st == 0x00 ? "OK" : "already-registered", st);
    }

    // Step 7b: AF_REGISTER EP2 (IPM profile 0x0108) + EP4 (TA profile 0x0107).
    // Mirrors working z-stack coordinator setup (3-EP layout, no Green Power EP242).
    auto register_ep = [](uint8_t ep, uint16_t profile, uint16_t device,
                           uint8_t device_ver, uint8_t latency, const char* label) {
        uint8_t pl[] = {
            ep,
            static_cast<uint8_t>(profile & 0xFF), static_cast<uint8_t>(profile >> 8),
            static_cast<uint8_t>(device  & 0xFF), static_cast<uint8_t>(device  >> 8),
            device_ver,
            latency,
            0x00,  // in_cluster_count
            0x00,  // out_cluster_count
        };
        MtFrame req{}; req.cmd0 = MT_SREQ(ZNP_AF); req.cmd1 = 0x00;
        req.payload = pl; req.payload_len = sizeof(pl);
        MtFrame rsp{};
        if (znp_sreq_retry(req, rsp, 3000, 3) && rsp.payload_len >= 1) {
            uint8_t st = rsp.payload[0];
            if (st == 0x00 || st == 0x08 || st == 0xB8) {
                ESP_LOGI(TAG, "AF_REGISTER %s %s", label,
                         st == 0x00 ? "OK" : "already-registered");
            } else {
                ESP_LOGW(TAG, "AF_REGISTER %s status=0x%02x (non-fatal)", label, st);
            }
        }
    };
    register_ep(2, 0x0108, 0x0005, 1, 0, "EP2 IPM");   // Industrial Plant Monitoring
    register_ep(4, 0x0107, 0x0005, 1, 0, "EP4 TA");    // Telecom Applications

    // Mirror z2m's endpoint set (zigbee-herdsman endpoints.ts) so ZB3
    // devices see the full "well-behaved coordinator" fingerprint. Some
    // devices (MiBoxer ZB3 remotes, Hue dimmers, TERNCY) refuse to join
    // if specific EPs are missing. No in/out cluster lists — we only
    // need the profile/device tuples visible to the joining device's
    // match_desc scan.
    register_ep(3,   0x0104, 0x0005, 1, 0, "EP3 HA");    // herdsman commit d0fb06c2
    register_ep(5,   0x0108, 0x0005, 1, 0, "EP5 IPM");   // alternate IPM slot
    register_ep(6,   0x0109, 0x0005, 1, 0, "EP6 AMI");   // Advanced Metering
    register_ep(8,   0x0104, 0x0005, 1, 0, "EP8 HA");
    register_ep(10,  0x0104, 0x0005, 1, 0, "EP10 HA");
    register_ep(11,  0x0104, 0x0400, 1, 0, "EP11 IASACE");
    register_ep(12,  0xc05e, 0x0005, 1, 0, "EP12 LL");   // ZLL
    register_ep(13,  0x0104, 0x0005, 1, 0, "EP13 OTA");
    register_ep(47,  0x0104, 0x0005, 1, 0, "EP47 HA-OTA");
    register_ep(0x6e,0x0104, 0x0005, 1, 0, "EP6E TERNCY");
    // EP242 Green Power — profile 0xa1e0. Mandatory for ZB3; many
    // non-GP devices still sniff for it during commissioning.
    register_ep(242, 0xa1e0, 0x0061, 0, 0, "EP242 GP");

    // Step 8: ZDO_MSG_CB_REGISTER for join and leave
    auto reg_cb = [](uint16_t cmd_id, const char* name) -> bool {
        uint8_t pl[2] = {static_cast<uint8_t>(cmd_id & 0xFF),
                         static_cast<uint8_t>(cmd_id >> 8)};
        MtFrame req{};
        req.cmd0 = MT_SREQ(ZNP_ZDO); req.cmd1 = 0x3E;
        req.payload = pl; req.payload_len = 2;
        MtFrame rsp{};
        if (!znp_sreq_retry(req, rsp, 3000, 3) || rsp.payload_len < 1 || rsp.payload[0] != 0x00) {
            ESP_LOGE("zigbee_mgr", "ZDO_MSG_CB_REGISTER 0x%04x %s failed", cmd_id, name);
            return false;
        }
        return true;
    };
    if (!reg_cb(0x00CA, "JOIN")) return false;
    if (!reg_cb(0x00C9, "LEAVE")) return false;

    s_init_done = true;
    ESP_LOGI(TAG, "ZNP coordinator ready");
    return true;
}

// ── ZNP Init Sequence (TDD Section 6.3) ──────────────────────────────────
// Caller is now responsible for `zigbee_pool_init()` + restore + any
// other consumers (device_shadow) BEFORE this call. Boot ordering moved
// up so `device_shadow_init` can iterate the already-loaded pool instead
// of re-reading NVS (03-F06).
bool zigbee_mgr_init() {
    // Legacy `zcl_converter_register_generated()` removed; decode and
    // encode flow through the zhc library (see zhc_adapter.h).
    znp_driver_init();

    s_zcl_queue = xQueueCreate(ZCL_QUEUE_DEPTH, sizeof(AfRawFrame));
    configASSERT(s_zcl_queue);
    // 8 KB: zhc pipeline puts DecodedMessage (~1.7 KB) + dispatch_from_zigbee
    // scratches + per-converter locals (lumi MI-struct TLV is ~1 KB on its
    // own) on this task's stack. 4 KB overflows on rotate/vibrate frames.
    xTaskCreate(zcl_attr_task, "zcl_attr", zhac::stack::kZclAttr, nullptr, 5, nullptr);

    znp_register_areq(MT_AREQ(ZNP_SYS), 0x80, on_reset_ind);
    znp_register_areq(MT_AREQ(ZNP_ZDO), 0xC0, on_state_change);

    // Wire zhc_adapter's hooks to our AF bridge (radio TX) + shadow
    // bridge (decode → device_shadow). Decls are file-scope via the
    // extern "C" block near the top of this file.
    zhc_send_bridge_register();
    zhc_shadow_bridge_register();
    zhc_configure_bridge_register();

    if (!coordinator_start()) return false;

    znp_register_areq(MT_AREQ(ZNP_ZDO), 0xC9, on_zdo_leave_ind); // ZDO_LEAVE_IND
    zigbee_interview_init();
    zigbee_identity_init();
    zigbee_configure_init();
    // AF_INCOMING_MSG cmd1 values (Z-Stack 3.x): 0x81 = standard,
    // 0x82 = extended (MSG_EXT, used by some stack builds for payloads
    // > 128 bytes). The dispatcher does exact-match only, so both must
    // be registered. Previously this was 0xFF, which is not a valid MT
    // AF command ID — no inbound ZCL traffic reached the interview /
    // identity paths (see log investigation 2026-04-15).
    znp_register_areq(MT_AREQ(ZNP_AF), 0x81, on_af_incoming_msg);
    znp_register_areq(MT_AREQ(ZNP_AF), 0x82, on_af_incoming_msg);

    return true;
}

bool zigbee_mgr_crashed() { return s_znp_crashed; }

// Re-run the coordinator startup without repeating one-time setup.
// AREQs registered by zigbee_mgr_init stay registered.
bool zigbee_mgr_reinit() {
    ESP_LOGI(TAG, "ZNP crash recovery: re-initialising coordinator");
    // Drop any stale JoinEvents queued before the crash — the NWKs they
    // carry may be invalid on the new network. Interview-path AREQ
    // subscriptions and the task itself persist across the reinit.
    zigbee_interview_flush_join_queue();
    return coordinator_start();
}

bool zigbee_force_recommission() {
    // The ZNP stick has its OWN internal NVS (PAN ID, network key,
    // device table, LOGICAL_TYPE). Our ESP-side zap_v0 erase does not
    // touch it. Wipe just the ZNP_HAS_CONFIGURED marker — `do_commission`
    // on next boot handles the rest: STARTUP_OPTION=0x03, soft reset
    // (which factory-resets the stick via its own boot flow), then full
    // BDB commissioning with fresh channel + net_key from zigbee_cfg.
    uint16_t len = nv_length(NV_ZNP_HAS_CONFIGURED);
    if (len == 0) {
        ESP_LOGW(TAG, "force_recommission: marker already absent");
        return true;
    }
    if (!nv_delete(NV_ZNP_HAS_CONFIGURED, len)) {
        ESP_LOGE(TAG, "force_recommission: nv_delete marker failed");
        return false;
    }
    ESP_LOGW(TAG, "force_recommission: ZNP_HAS_CONFIGURED wiped — "
                  "next boot will run full commissioning");
    return true;
}
