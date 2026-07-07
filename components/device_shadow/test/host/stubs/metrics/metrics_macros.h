// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the metrics instrumentation macros. device_shadow_process is
// wrapped in _METRIC_TIMER_SCOPE(METRIC_SHADOW_PROCESS); on the host we want no
// counters/histograms, so every macro expands to nothing (the metric-id token
// argument is discarded, so it never needs a definition). A handful of the
// other common metric macros are stubbed defensively in case the component
// grows new instrumentation.
#pragma once

#define _METRIC_TIMER_SCOPE(id)
#define METRIC_TIMER_SCOPE(id)
#define _METRIC_INC(id)
#define METRIC_INC(id)
#define _METRIC_ADD(id, n)
#define METRIC_ADD(id, n)
#define _METRIC_SET(id, n)
#define METRIC_SET(id, n)
