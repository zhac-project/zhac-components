// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stub for metrics/metrics_macros.h. hap_session.cpp only ever calls
// _METRIC_COUNTER_INC(name, val) (window-full drops + duplicate-seq drops).
// Expand it to a no-op so the metric names are consumed as macro arguments and
// never reach the compiler — no counters, no linkage against the real metrics
// registry. If hap_session grows new metric calls, add matching no-op macros
// here.
#pragma once

#define _METRIC_COUNTER_INC(name, val) ((void)0)
