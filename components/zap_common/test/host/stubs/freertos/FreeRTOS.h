// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Minimal host stub of FreeRTOS for zap_common's header-only
// sys_metrics.h. Only the symbols sys_metrics_sample_cpu_pct touches
// are provided; the run-time / idle-task counters are driven by test
// globals (see freertos/task.h) so the CPU%-baseline maths can be
// exercised deterministically off-target.
#pragma once

#include <cstdint>

// sys_metrics.h reads portNUM_PROCESSORS and portGET_RUN_TIME_COUNTER_VALUE().
#ifndef portNUM_PROCESSORS
#define portNUM_PROCESSORS 2
#endif

#define pdFALSE 0

// Scripted run-time counter — the test sets this before each sample.
extern uint32_t g_stub_run_time_counter;
#define portGET_RUN_TIME_COUNTER_VALUE() (g_stub_run_time_counter)
