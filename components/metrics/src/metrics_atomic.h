// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// Lock-free min/max via compare-exchange loops. Uses relaxed ordering
// throughout — metrics snapshots tolerate small races and don't
// synchronise with other data.

#include <atomic>

namespace metrics::detail {

template <typename T>
inline void atomic_min(std::atomic<T>& target, T value) noexcept {
    T cur = target.load(std::memory_order_relaxed);
    while (value < cur) {
        if (target.compare_exchange_weak(cur, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
            return;
        }
    }
}

template <typename T>
inline void atomic_max(std::atomic<T>& target, T value) noexcept {
    T cur = target.load(std::memory_order_relaxed);
    while (value > cur) {
        if (target.compare_exchange_weak(cur, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
            return;
        }
    }
}

}  // namespace metrics::detail
