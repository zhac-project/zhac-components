// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// device_cmd, cut two: rename, permit join, remove. Kept apart from
// device_cmd.cpp so the rule engine's host suite (which links only the
// attribute path) does not have to fake the radio, the store and the backend.
#include "device_cmd.h"

#include <cstdio>
#include <cstring>

#include "device_backend.h"
#include "device_shadow.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_mgr.h"
#include "zigbee_pool.h"

static const char* TAG = "device_cmd";

// ── Rename ──────────────────────────────────────────────────────────────

static DevCmdChangedFn s_changed = nullptr;
void device_cmd_set_changed_hook(DevCmdChangedFn fn) { s_changed = fn; }

DevCmdResult device_cmd_rename(uint64_t ieee, const char* name) {
    if (!ieee || !name) return DEVCMD_BAD_ARGS;
    const size_t n = std::strlen(name);
    if (n == 0 || n >= sizeof(ZapDevice::friendly_name)) return DEVCMD_BAD_NAME;
    for (const char* c = name; *c; c++) {
        if (*c == '"' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20) return DEVCMD_BAD_NAME;
    }
    ZapDevice snap{};
    bool found = false;
    zigbee_pool_lock();
    if (ZapDevice* d = pool_find_by_ieee(ieee)) {
        std::snprintf(d->friendly_name, sizeof(d->friendly_name), "%s", name);
        snap  = *d;
        found = true;
    }
    zigbee_pool_unlock();
    if (!found) return DEVCMD_NOT_FOUND;
    // On the snapshot, outside the lock: mark_dirty's table-full fallback writes flash.
    zap_store_mark_dirty(&snap, ZAP_PERSIST_HIGH);
    if (s_changed) s_changed(ieee);
    return DEVCMD_OK;
}

// ── Permit join ─────────────────────────────────────────────────────────

static int64_t s_permit_deadline_us = 0;

DevCmdResult device_cmd_permit_join(uint8_t secs) {
    if (secs == 255) {
        ESP_LOGW(TAG, "permit join 255 (permanent) clamped to 254 s");
        secs = 254;
    }
    // A failed open leaves the previous deadline alone: it must not report the
    // network as closed while an earlier window is genuinely still open.
    if (!zigbee_permit_join(secs)) return DEVCMD_RADIO;
    s_permit_deadline_us = secs ? esp_timer_get_time() + static_cast<int64_t>(secs) * 1000000LL : 0;
    ESP_LOGI(TAG, "permit join %us", static_cast<unsigned>(secs));
    return DEVCMD_OK;
}

void device_cmd_permit_join_status(bool* open, int* remaining_s) {
    const int64_t now  = esp_timer_get_time();
    const bool    is_open = s_permit_deadline_us > now;
    if (open)        *open = is_open;
    if (remaining_s) *remaining_s = is_open ? static_cast<int>((s_permit_deadline_us - now + 999999LL) / 1000000LL) : 0;
}

// ── Remove ──────────────────────────────────────────────────────────────

DevCmdResult device_cmd_remove(uint64_t ieee, bool hard) {
    if (!ieee) return DEVCMD_BAD_ARGS;
    ZapDevice snap{};
    bool found = false;
    zigbee_pool_lock();
    if (ZapDevice* d = pool_find_by_ieee(ieee)) {
        if (!hard) zap_dev_mark_removed(d);
        snap  = *d;
        found = true;
    }
    zigbee_pool_unlock();
    if (!found && !hard) return DEVCMD_NOT_FOUND;

    if (found) {
        // Tell the device to go. Best effort: a sleepy or dead device cannot
        // answer, and a delete that fails because of that is useless (z2m
        // behaves the same). A backend that owns removal does the leave itself.
        DeviceBackend* b = hard ? device_backend_find(PROTO_ZIGBEE) : nullptr;
        if (b && b->remove_device) {
            b->remove_device(ieee);
        } else if (snap.nwk_addr) {
            zigbee_leave_req(snap.nwk_addr, ieee);
        }
    }
    if (!hard) {
        zap_store_mark_dirty(&snap, ZAP_PERSIST_LOW);
        return DEVCMD_OK;
    }
    zap_store_delete_device(ieee);
    device_shadow_remove(ieee);
    zhac_adapter_invalidate_def_cache(ieee);
    zhac_adapter_fallback_clear(ieee);
    zigbee_pool_remove(ieee);
    ESP_LOGI(TAG, "removed 0x%016llX (hard)", static_cast<unsigned long long>(ieee));
    return DEVCMD_OK;
}
