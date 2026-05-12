// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "device_shadow.h"
#include <cstring>

static bool key_eq(const char* a, const char* b) {
    return strncmp(a, b, ATTR_KEY_MAX) == 0;
}

extern "C" uint8_t shadow_pipeline_filter(const DeviceConfig* cfg,
                                           const ZclAttribute* in, uint8_t in_count,
                                           ZclAttribute* out, uint8_t max_out)
{
    uint8_t out_count = 0;
    for (uint8_t i = 0; i < in_count && out_count < max_out; i++) {
        bool filtered = false;
        for (uint8_t f = 0; f < cfg->filtered_count; f++) {
            if (key_eq(in[i].key, cfg->filtered[f])) {
                filtered = true;
                break;
            }
        }
        if (!filtered) {
            out[out_count++] = in[i];
        }
    }
    return out_count;
}

extern "C" bool shadow_pipeline_throttle_pass(DeviceConfig* cfg,
                                               uint32_t* last_ms,
                                               uint32_t now_ms)
{
    if (cfg->throttle_ms == 0) return true;
    if (*last_ms == 0 || (now_ms - *last_ms) >= cfg->throttle_ms) {
        *last_ms = now_ms;
        return true;
    }
    return false;
}

extern "C" int8_t shadow_pipeline_debounce_bypass(const DeviceConfig* cfg,
                                                    const PendingState* ps,
                                                    const ZclAttribute* attr)
{
    bool is_ignore = false;
    for (uint8_t i = 0; i < cfg->debounce_ignore_count; i++) {
        if (key_eq(attr->key, cfg->debounce_ignore[i])) {
            is_ignore = true;
            break;
        }
    }
    if (!is_ignore) return -1;

    for (uint8_t i = 0; i < ps->pending_count; i++) {
        if (key_eq(ps->pending[i].key, attr->key)) {
            bool same_val = (ps->pending[i].int_val == attr->int_val);
            if (same_val) return -1;
            return (int8_t)(i + 1);
        }
    }
    return 0;
}

extern "C" void shadow_pipeline_merge_pending(const DeviceConfig* cfg,
                                               PendingState* ps,
                                               const ZclAttribute* attrs,
                                               uint8_t count)
{
    (void)cfg;
    for (uint8_t i = 0; i < count; i++) {
        bool found = false;
        for (uint8_t j = 0; j < ps->pending_count; j++) {
            if (key_eq(ps->pending[j].key, attrs[i].key)) {
                ps->pending[j] = attrs[i];
                found = true;
                break;
            }
        }
        if (!found && ps->pending_count < 32) {
            ps->pending[ps->pending_count++] = attrs[i];
        }
    }
}

extern "C" uint8_t shadow_pipeline_flush_pending(PendingState* ps,
                                                   ZclAttribute* out,
                                                   uint8_t max_out)
{
    uint8_t n = (ps->pending_count < max_out) ? ps->pending_count : max_out;
    for (uint8_t i = 0; i < n; i++) out[i] = ps->pending[i];
    if (n < ps->pending_count) {
        uint8_t remaining = ps->pending_count - n;
        for (uint8_t i = 0; i < remaining; i++) {
            ps->pending[i] = ps->pending[n + i];
        }
        ps->pending_count = remaining;
    } else {
        ps->pending_count = 0;
    }
    return n;
}
