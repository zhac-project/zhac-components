// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// nvs_checked.h — accumulating NVS-error check for multi-op sequences.
//
// P1 findings review: several persistence paths chained nvs_set_* /
// nvs_commit / nvs_erase_* calls and ignored every return, so a failed
// write was reported (and logged) as success. This helper makes the
// honest pattern cheap enough to use everywhere:
//
//   esp_err_t acc = ESP_OK;
//   nvs_seq(&acc, nvs_set_blob(...), TAG, "set_blob devpool");
//   nvs_seq(&acc, nvs_commit(...),   TAG, "commit devpool");
//   return acc == ESP_OK;
//
// Semantics:
//   - every failing op is logged (ESP_LOGE, cold path — per-op logging
//     beats log-once when diagnosing which op of a sequence died);
//   - `*acc` keeps the FIRST error of the sequence (later failures are
//     logged but do not overwrite it);
//   - the call returns `r` unchanged, so callers can still branch on the
//     individual op (e.g. skip a version-marker write after a failed
//     erase_all) while the accumulator tracks overall success.
#pragma once

#include "esp_err.h"
#include "esp_log.h"

static inline esp_err_t nvs_seq(esp_err_t* acc, esp_err_t r,
                                const char* tag, const char* op) {
    if (r != ESP_OK) {
        ESP_LOGE(tag, "nvs %s: %s", op, esp_err_to_name(r));
        if (*acc == ESP_OK) *acc = r;
    }
    return r;
}
