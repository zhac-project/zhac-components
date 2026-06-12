// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host tests for nvs_checked.h (P1-T8): accumulating NVS-error check.
// Contract under test:
//   - acc keeps the FIRST error of a sequence;
//   - every op is attempted (no short-circuit) and every failing op is
//     logged exactly once (counted via the local ESP_LOGE stub);
//   - nvs_seq returns its `r` argument unchanged (passthrough), even
//     when acc already holds an earlier failure;
//   - acc == nullptr is allowed (best-effort sites): per-op logging and
//     passthrough still work, nothing is dereferenced.
#include "nvs_checked.h"

#include <cstdio>

int g_esp_loge_count = 0;   // bumped by the ESP_LOGE stub

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Stand-in for an nvs_* call: side effect proves the op was attempted.
static int s_ops_attempted = 0;
static esp_err_t op(esp_err_t r) {
    s_ops_attempted++;
    return r;
}

int main() {
    static const char* TAG = "nvs_checked_test";
    // Distinct nonzero codes — values don't matter, identity does.
    constexpr esp_err_t kErrA = 0x1102;  // "first failure"
    constexpr esp_err_t kErrB = 0x1108;  // "second failure"

    // ── 1. all-OK sequence ────────────────────────────────────────────────
    {
        s_ops_attempted = 0;
        g_esp_loge_count = 0;
        esp_err_t acc = ESP_OK;
        CHECK(nvs_seq(&acc, op(ESP_OK), TAG, "op1") == ESP_OK,
              "OK op passes through ESP_OK");
        CHECK(nvs_seq(&acc, op(ESP_OK), TAG, "op2") == ESP_OK,
              "second OK op passes through ESP_OK");
        CHECK(acc == ESP_OK, "all-OK sequence leaves acc == ESP_OK");
        CHECK(g_esp_loge_count == 0, "all-OK sequence logs nothing");
        CHECK(s_ops_attempted == 2, "all-OK: both ops attempted");
    }

    // ── 2. OK, FAIL, OK — the task-card sequence ──────────────────────────
    {
        s_ops_attempted = 0;
        g_esp_loge_count = 0;
        esp_err_t acc = ESP_OK;
        esp_err_t r1 = nvs_seq(&acc, op(ESP_OK), TAG, "op1");
        esp_err_t r2 = nvs_seq(&acc, op(kErrA),  TAG, "op2");
        esp_err_t r3 = nvs_seq(&acc, op(ESP_OK), TAG, "op3");
        CHECK(acc == kErrA, "OK,FAIL,OK: acc == the failure");
        CHECK(r1 == ESP_OK && r2 == kErrA && r3 == ESP_OK,
              "OK,FAIL,OK: each return passes through unchanged");
        CHECK(s_ops_attempted == 3,
              "OK,FAIL,OK: all ops attempted (no short-circuit)");
        CHECK(g_esp_loge_count == 1, "OK,FAIL,OK: exactly one error logged");
    }

    // ── 3. two failures: first wins, both logged, passthrough holds ──────
    {
        s_ops_attempted = 0;
        g_esp_loge_count = 0;
        esp_err_t acc = ESP_OK;
        nvs_seq(&acc, op(kErrA), TAG, "op1");
        esp_err_t r2 = nvs_seq(&acc, op(kErrB), TAG, "op2");
        CHECK(acc == kErrA, "two failures: acc keeps the FIRST error");
        CHECK(r2 == kErrB,
              "two failures: passthrough returns r even with acc set");
        CHECK(g_esp_loge_count == 2, "two failures: every failing op logged");
        CHECK(s_ops_attempted == 2, "two failures: both ops attempted");
    }

    // ── 4. acc already failed: OK op neither clears nor re-logs ──────────
    {
        g_esp_loge_count = 0;
        esp_err_t acc = kErrA;
        esp_err_t r = nvs_seq(&acc, op(ESP_OK), TAG, "op1");
        CHECK(r == ESP_OK && acc == kErrA,
              "OK op after failure: passthrough OK, acc untouched");
        CHECK(g_esp_loge_count == 0, "OK op after failure: no log");
    }

    // ── 5. nullptr acc: log-only mode for best-effort sites ──────────────
    {
        s_ops_attempted = 0;
        g_esp_loge_count = 0;
        esp_err_t r1 = nvs_seq(nullptr, op(kErrA),  TAG, "op1");
        esp_err_t r2 = nvs_seq(nullptr, op(ESP_OK), TAG, "op2");
        CHECK(r1 == kErrA && r2 == ESP_OK,
              "nullptr acc: passthrough returns r unchanged, no crash");
        CHECK(g_esp_loge_count == 1, "nullptr acc: failing op still logged");
        CHECK(s_ops_attempted == 2, "nullptr acc: both ops attempted");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS",
           s_failures);
    return s_failures ? 1 : 0;
}
