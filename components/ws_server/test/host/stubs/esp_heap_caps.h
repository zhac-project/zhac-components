// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM   1
#define MALLOC_CAP_8BIT     2
#define MALLOC_CAP_INTERNAL 4
// PSRAM can be made full (g_psram_full: a PSRAM allocation fails); allocations that ask for internal RAM are counted.
inline bool g_psram_full = false;
inline int  g_internal_allocs = 0;
static inline void* heap_caps_malloc(size_t n, int caps) {
    if (caps & MALLOC_CAP_SPIRAM) {
        if (g_psram_full) return nullptr;
    } else if (caps & MALLOC_CAP_INTERNAL) {
        g_internal_allocs++;
    }
    return malloc(n);
}
