// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host test for sys_metrics.h (P4-T28, FINDINGS §8): the CPU%-baseline
// state must be CALLER-OWNED, not a per-TU header static. Two contexts
// sampled in interleaved order must not cross-corrupt each other's
// rolling window, and the first sample on a fresh context must report 0
// (no baseline) rather than a garbage delta.
//
// FreeRTOS run-time / idle counters are scripted via the stub globals
// (see stubs/freertos/*). CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is on
// (stubs/sdkconfig.h) so the real delta maths runs.

#include "sys_metrics.h"

#include <cstdint>
#include <cstdio>

// Definitions for the stub-declared scripted counters.
uint32_t g_stub_run_time_counter = 0;
uint32_t g_stub_idle0_counter    = 0;
uint32_t g_stub_idle1_counter    = 0;

static int s_failures = 0;
#define CHECK(cond, msg) do {                                         \
    if (cond) { printf("PASS: %s\n", msg); }                          \
    else      { printf("FAIL: %s\n", msg); s_failures++; }            \
} while (0)

// Advance the scripted clock + idle counters, then sample `ctx`.
static void sample(sys_metrics_cpu_ctx_t& ctx, uint32_t tm,
                   uint32_t idle0, uint32_t idle1,
                   uint8_t& c0, uint8_t& c1) {
    g_stub_run_time_counter = tm;
    g_stub_idle0_counter    = idle0;
    g_stub_idle1_counter    = idle1;
    sys_metrics_sample_cpu_pct(ctx, c0, c1);
}

int main() {
    // ── 1. Fresh context: first sample reports 0 (no baseline) ──────────
    {
        sys_metrics_cpu_ctx_t ctx{};
        uint8_t c0 = 0xFF, c1 = 0xFF;
        sample(ctx, 1000, 500, 500, c0, c1);
        CHECK(c0 == 0 && c1 == 0,
              "fresh ctx: first sample is 0 (no garbage delta)");
    }

    // ── 2. Steady-state delta maths on one context ──────────────────────
    {
        sys_metrics_cpu_ctx_t ctx{};
        uint8_t c0 = 0, c1 = 0;
        sample(ctx, 0,    0,    0,    c0, c1);   // seed
        // Over the next window of 100 ticks, core0 idle advanced 25 (so
        // 75% busy) and core1 idle advanced 100 (so 0% busy).
        sample(ctx, 100,  25,   100,  c0, c1);
        CHECK(c0 == 75, "single ctx: core0 reports 75% busy");
        CHECK(c1 == 0,  "single ctx: core1 reports 0% busy");
    }

    // ── 3. Two contexts on DIFFERENT cadences do not cross-corrupt ──────
    // This is the core regression: with the old per-TU static, ctxA and
    // ctxB would have shared one baseline, so interleaving them produced
    // bogus deltas. With caller-owned ctx, each keeps its own window.
    {
        sys_metrics_cpu_ctx_t ctxA{};   // e.g. heartbeat cadence
        sys_metrics_cpu_ctx_t ctxB{};   // e.g. REST /status cadence
        uint8_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;

        // Seed both at the same clock origin.
        sample(ctxA, 0, 0, 0, a0, a1);
        sample(ctxB, 0, 0, 0, b0, b1);

        // A advances: tm 0→100, idle0 0→10 → core0 90% busy.
        sample(ctxA, 100, 10, 100, a0, a1);
        // Interleave B with a DIFFERENT window: tm 0→200, idle0 0→100 →
        // core0 50% busy. If B read A's baseline (tm=100) it would see
        // dt=100 and a wrong percentage.
        sample(ctxB, 200, 100, 200, b0, b1);

        CHECK(a0 == 90, "ctxA core0 == 90% (its own window)");
        CHECK(b0 == 50, "ctxB core0 == 50% (its own window, not A's)");

        // Continue A independently; B's last baseline must be untouched
        // by A's samples. A: tm 100→300 (dt 200), idle0 10→110 (d=100)
        // → 50% busy.
        sample(ctxA, 300, 110, 300, a0, a1);
        CHECK(a0 == 50, "ctxA core0 next window == 50% (no B interference)");

        // And B resumes from ITS own baseline (tm=200), not A's (tm=300):
        // tm 200→400 (dt 200), idle0 100→150 (d=50) → 75% busy.
        sample(ctxB, 400, 150, 400, b0, b1);
        CHECK(b0 == 75, "ctxB core0 resumes from own baseline == 75%");
    }

    // ── 4. Per-core independence within one context ─────────────────────
    {
        sys_metrics_cpu_ctx_t ctx{};
        uint8_t c0 = 0, c1 = 0;
        sample(ctx, 0,   0,  0,  c0, c1);
        // dt=100; core0 idle +100 (0% busy), core1 idle +0 (100% busy).
        sample(ctx, 100, 100, 0, c0, c1);
        CHECK(c0 == 0 && c1 == 100,
              "single ctx: cores tracked independently (0% / 100%)");
    }

    printf("\n%s — %d failure(s)\n", s_failures ? "FAILED" : "ALL PASS",
           s_failures);
    return s_failures ? 1 : 0;
}
