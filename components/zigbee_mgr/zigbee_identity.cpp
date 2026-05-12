// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// zigbee_identity.cpp — late identity enrichment + re-match (Step 2).
//
// Design goals:
//   - Never block the RX task. Identity snapshots are posted to a queue with
//     zero timeout; a dedicated worker consumes them.
//   - Persist a device at most once per identity change, not per frame.
//   - Re-match is triggered only when identity actually changes AND the
//     device's previous support_state was not already MATCHED. The match is
//     run inline here for now; the deferred configure queue (Step 3) will
//     later take over the configure side.
//
// The parser lives in zigbee_interview_utils.cpp and handles both ZCL
// Read-Attributes-Response and Report-Attributes frames.

#include "zigbee_identity.h"
#include "zigbee_interview_utils.h"
#include "zigbee_pool.h"
#include "zigbee_mgr.h"
#include "zap_store.h"
#include "zap_common.h"
#include "zhc_adapter.h"
#include "zigbee_configure_queue.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstring>
#include "task_stacks.h"

static const char* TAG = "zigbee_identity";

struct IdentityUpdate {
    uint16_t nwk;
    char     model_id[34];
    char     manufacturer_name[34];
    uint16_t manufacturer_code;
    uint8_t  has_model;
    uint8_t  has_mfg;
    uint8_t  has_code;
};
static_assert(sizeof(IdentityUpdate) <= 80);

static QueueHandle_t s_q = nullptr;

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void zigbee_identity_on_af_incoming(const uint8_t* af, uint8_t af_len) {
    if (!s_q || af_len < 17) return;

    // AF_INCOMING_MSG layout (matches zigbee_mgr::on_af_incoming_msg):
    //   group(2) cluster(2) src_nwk(2) src_ep(1) dst_ep(1)
    //   broadcast(1) lqi(1) security(1) timestamp(4) trans_seq(1)
    //   data_len(1) data[...]
    const uint16_t cluster = le16(af + 2);
    if (cluster != 0x0000) return;

    const uint16_t src_nwk = le16(af + 4);
    const uint8_t  data_len = af[16];
    if ((uint16_t)17 + data_len > af_len) return;

    IdentityUpdate u{};
    u.nwk = src_nwk;
    const bool any = zigbee_parse_basic_identity(
        af + 17, data_len,
        u.model_id, sizeof(u.model_id),
        u.manufacturer_name, sizeof(u.manufacturer_name),
        &u.manufacturer_code);
    if (!any) return;

    u.has_model = u.model_id[0] != '\0';
    u.has_mfg   = u.manufacturer_name[0] != '\0';
    u.has_code  = (u.manufacturer_code != 0);

    // Zero-timeout send. Dropping a duplicate update is harmless: the device
    // will resend identity on its next report cycle.
    (void)xQueueSend(s_q, &u, 0);
}

// Update fields on the device when they actually differ. Returns true if
// anything changed — caller persists and considers re-matching.
static bool apply_identity(ZapDevice* dev, const IdentityUpdate& u) {
    bool changed = false;
    if (u.has_model && strncmp(dev->model_id, u.model_id, sizeof(dev->model_id)) != 0) {
        strncpy(dev->model_id, u.model_id, sizeof(dev->model_id) - 1);
        dev->model_id[sizeof(dev->model_id) - 1] = '\0';
        changed = true;
    }
    if (u.has_mfg && strncmp(dev->manufacturer_name, u.manufacturer_name,
                              sizeof(dev->manufacturer_name)) != 0) {
        strncpy(dev->manufacturer_name, u.manufacturer_name,
                sizeof(dev->manufacturer_name) - 1);
        dev->manufacturer_name[sizeof(dev->manufacturer_name) - 1] = '\0';
        changed = true;
    }
    if (u.has_code && dev->manufacturer_code != u.manufacturer_code) {
        dev->manufacturer_code = u.manufacturer_code;
        changed = true;
    }
    return changed;
}

static void task_identity(void*) {
    IdentityUpdate u{};
    for (;;) {
        if (xQueueReceive(s_q, &u, portMAX_DELAY) != pdTRUE) continue;

        // Copy out the fields we'll need for persistence + re-match while
        // holding the pool lock, then drop the lock before running the
        // matcher/configure which can be heavy.
        uint64_t ieee = 0;
        uint16_t nwk  = 0;
        uint8_t  primary_ep = 0;
        uint8_t  interview_state = (uint8_t)InterviewState::NONE;
        uint8_t  support_state   = (uint8_t)SupportState::UNKNOWN;
        bool     should_rematch  = false;
        bool     state_changed   = false;

        zigbee_pool_lock();
        ZapDevice* dev = pool_find_by_nwk(u.nwk);
        if (dev) {
            bool changed = apply_identity(dev, u);

            // Promote IDENTITY_PENDING → IDENTITY_READY once we have either
            // model or manufacturer. Stale TOPOLOGY_READY devices also get
            // promoted, since the interview would have tried Basic read.
            if ((dev->interview_state == (uint8_t)InterviewState::IDENTITY_PENDING ||
                 dev->interview_state == (uint8_t)InterviewState::TOPOLOGY_READY) &&
                (dev->model_id[0] != '\0' || dev->manufacturer_name[0] != '\0')) {
                dev->interview_state = (uint8_t)InterviewState::IDENTITY_READY;
                state_changed = true;
            }

            // Re-match only when identity actually changed and we don't
            // already have a working converter binding (avoid churn on
            // already-MATCHED devices whose reports just confirmed identity).
            if ((changed || state_changed) &&
                dev->support_state != (uint8_t)SupportState::MATCHED) {
                should_rematch = true;
            }

            if (changed || state_changed) {
                ieee = dev->ieee_addr;
                nwk  = dev->nwk_addr;
                primary_ep = (dev->endpoint_count > 0) ? dev->endpoints[0] : 1;
                interview_state = dev->interview_state;
                support_state   = dev->support_state;
                // Persist a copy: zap_store_save_device must NOT be called
                // while iterating pool or holding other nested locks.
                ZapDevice snap = *dev;
                zigbee_pool_unlock();
                zap_store_mark_dirty(&snap, ZAP_PERSIST_LOW);
            } else {
                zigbee_pool_unlock();
                continue;
            }
        } else {
            zigbee_pool_unlock();
            continue;
        }

        if (state_changed) {
            ESP_LOGI(TAG, "late identity: nwk=0x%04x model='%s' mfg='%s' → IDENTITY_READY",
                     nwk, u.model_id, u.manufacturer_name);
        }
        // model_id / manufacturer_name just changed — drop the adapter's
        // cached def pointer so the next frame re-runs find_definition
        // against the freshly-enriched identity.
        zhac_adapter_invalidate_def_cache(ieee);
        if (!should_rematch) continue;

        // Re-match outside the pool lock. Snapshot the device again — the
        // matcher takes a const ref but we must not access the pool entry
        // concurrently with a rejoin that could reslot it.
        zigbee_pool_lock();
        ZapDevice* d2 = pool_find_by_ieee(ieee);
        if (!d2) { zigbee_pool_unlock(); continue; }
        ZapDevice snap = *d2;
        zigbee_pool_unlock();

        const bool supported = zhac_adapter_has_def(snap.ieee_addr,
                                                     snap.model_id,
                                                     snap.manufacturer_name);
        uint8_t new_support = supported ? (uint8_t)SupportState::MATCHED
                                         : (uint8_t)SupportState::UNMATCHED;
        if (new_support == support_state && !supported) continue;  // no news

        if (supported) {
            ESP_LOGI(TAG, "re-match: nwk=0x%04x model='%s' mfg='%s' → MATCHED",
                     nwk, snap.model_id, snap.manufacturer_name);
        }

        // Write back match outcome and hand configure off to the deferred
        // queue. The queue dedups on ConfigureState::DONE so a device that
        // was already configured on a prior boot won't be re-bound.
        bool enqueue = false;
        zigbee_pool_lock();
        ZapDevice* d3 = pool_find_by_ieee(ieee);
        if (d3) {
            d3->support_state = new_support;
            if (supported && d3->configure_state != (uint8_t)ConfigureState::DONE) {
                d3->configure_state    = (uint8_t)ConfigureState::PENDING;
                d3->configure_attempts = 0;
                enqueue = true;
            } else if (!supported) {
                d3->configure_state = (uint8_t)ConfigureState::PENDING;
            }
            ZapDevice snap2 = *d3;
            zigbee_pool_unlock();
            zap_store_mark_dirty(&snap2, ZAP_PERSIST_LOW);
        } else {
            zigbee_pool_unlock();
        }
        if (enqueue) zigbee_configure_enqueue(ieee);
        (void)primary_ep;
        (void)interview_state;
    }
}

void zigbee_identity_init() {
    if (s_q) return;  // idempotent
    s_q = xQueueCreate(8, sizeof(IdentityUpdate));
    configASSERT(s_q);
    xTaskCreate(task_identity, "zb_identity", zhac::stack::kZbIdentity, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "identity task ready");
}
