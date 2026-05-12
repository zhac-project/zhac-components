// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Component-private accessors so other TUs in `components/metrics/`
// (exporters, dump, …) can iterate the descriptor table without each
// re-expanding the X-macro registry and duplicating the flash cost.

#include "metrics/metrics_types.h"
#include <cstddef>

namespace metrics::detail {

const MetricDescriptor* descriptors() noexcept;
size_t                  descriptor_count() noexcept;

}  // namespace metrics::detail
