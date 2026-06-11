// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal cross-TU hooks between zhc_adapter.cpp and
// zhc_adapter_fallback.cpp. NOT part of the public component API
// (include/zhc_adapter.h) — firmware code must not include this.

#pragma once

namespace zhc_adapter_internal {

// Clear every per-IEEE cached definition pointer (IeeeSlot.cached_def /
// .cached_supplement) that points into [begin, end). The fallback pool
// calls this — while holding its pool mutex — right before a victim
// slot's storage is repurposed for a different device (LRU eviction)
// or dropped (clear), so the evicted device's frames can never keep
// decoding through storage that now describes the NEW device. The range
// must span the victim slot's ENTIRE built-def storage (both halves of
// the A/B double buffer).
void invalidate_cached_defs_in(const void* begin, const void* end);

}  // namespace zhc_adapter_internal
