// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host-test sdkconfig stub. Enables the run-time-stats path so
// sys_metrics.h's CPU%-baseline maths is actually exercised (not the
// disabled c0=c1=0 stub) when test_sys_metrics.cpp drives scripted
// counters through two independent contexts.
#pragma once

#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1
