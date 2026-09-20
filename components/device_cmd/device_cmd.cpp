// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "device_cmd.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "device_shadow.h"
#include "zap_common.h"
#include "zcl_attribute.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

const char* device_cmd_result_str(DevCmdResult r) {
    switch (r) {
        case DEVCMD_OK:           return "ok";
        case DEVCMD_BAD_ARGS:     return "missing key or value";
        case DEVCMD_BAD_VALUE:    return "value must be bool / number / string";
        case DEVCMD_NOT_FOUND:    return "device not found";
        case DEVCMD_NO_CONVERTER: return "no zhc converter";
        case DEVCMD_BAD_NAME:     return "name must be 1-29 characters without quotes, backslashes or control characters";
        case DEVCMD_RADIO:        return "radio refused";
    }
    return "failed";
}

DevCmdValue device_cmd_number(double d) {
    if (d == std::floor(d) && std::fabs(d) < 9.2e18) return device_cmd_int(static_cast<int64_t>(d));
    return device_cmd_float(d);
}

DevCmdResult device_cmd_set_attr(uint64_t ieee, uint8_t ep, const char* key, const DevCmdValue* v) {
    if (!key || !key[0] || !v) return DEVCMD_BAD_ARGS;
    if (v->kind == DEVCMD_STR && !v->s) return DEVCMD_BAD_VALUE;
    if (v->kind > DEVCMD_STR) return DEVCMD_BAD_VALUE;
    if (v->kind == DEVCMD_FLOAT && v->f != v->f) return DEVCMD_BAD_VALUE;   // NaN

    // Copy what the send needs under the pool lock, then let go of it: the
    // radio dispatch blocks, and device reports must not wait on it.
    uint64_t ieee_cp = 0; uint16_t nwk_cp = 0; uint8_t ep_cp = ep;
    char model_cp[64], manu_cp[64];
    zigbee_pool_lock();
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) { zigbee_pool_unlock(); return DEVCMD_NOT_FOUND; }
    ieee_cp = dev->ieee_addr;
    nwk_cp  = dev->nwk_addr;
    if (ep_cp == 0) ep_cp = dev->endpoints[0] ? dev->endpoints[0] : 1;
    std::snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    std::snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    zigbee_pool_unlock();

    bool ok = false;
    switch (v->kind) {
        case DEVCMD_BOOL:
            ok = zhac_adapter_send_bool(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, v->b);
            break;
        case DEVCMD_INT:
            ok = v->i >= 0
                ? zhac_adapter_send_uint(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, static_cast<uint64_t>(v->i))
                : zhac_adapter_send_number(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, static_cast<double>(v->i));
            break;
        case DEVCMD_FLOAT:
            ok = zhac_adapter_send_float(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, v->f);
            break;
        case DEVCMD_STR:
            ok = zhac_adapter_send_string(ieee_cp, model_cp, manu_cp, nwk_cp, ep_cp, key, v->s);
            break;
    }
    if (!ok) return DEVCMD_NO_CONVERTER;

    // Optimistic shadow: many devices (the whole no-report Tuya class) send
    // nothing back after obeying, so without this the UI and the cloud show
    // the old value until the next physical change. A real report overrides.
    switch (v->kind) {
        case DEVCMD_BOOL:
            device_shadow_update_optimistic(ieee_cp, key, VAL_BOOL, v->b ? 1 : 0);
            break;
        case DEVCMD_INT: {
            const uint8_t vt = (std::strcmp(key, "state") == 0) ? VAL_BOOL : VAL_INT;
            device_shadow_update_optimistic(ieee_cp, key, vt, static_cast<int32_t>(v->i));
            break;
        }
        case DEVCMD_FLOAT:
            device_shadow_update_optimistic(ieee_cp, key, VAL_FLOAT, static_cast<int32_t>(std::lround(v->f * 100.0)));
            break;
        case DEVCMD_STR:
            break;   // the shadow holds numbers; the device's report supplies text
    }
    return DEVCMD_OK;
}

