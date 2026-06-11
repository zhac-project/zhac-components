// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host-test heap_caps shim — capability flags ignored, plain malloc/calloc.
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x0400
#define MALLOC_CAP_8BIT   0x0004

static inline void* heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}
static inline void* heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    (void)caps;
    return calloc(n, size);
}
