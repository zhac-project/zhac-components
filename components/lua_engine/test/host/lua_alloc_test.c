// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// lua_alloc.c's heap choice (research 2026-11-18 A7): a block at or above the internal-RAM threshold lives in
// PSRAM. A realloc asking only for 8-bit memory kept a small block growing past the threshold in internal RAM, and
// moved a big block PSRAM could not grow into internal RAM, as IDF tries that heap first. Built twice: threshold
// 512 (the old P4 setting) and 0 (P4 and S31 now).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lua_alloc.h"

int stub_spiram_full = 0;
struct hdr { int heap; size_t size; long pad; };
static uint32_t caps_of(int heap) {
    return MALLOC_CAP_8BIT | (heap == HEAP_SPIRAM ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
}
static void* block_in(int heap, size_t size) {
    if (heap == HEAP_SPIRAM && stub_spiram_full) return NULL;
    struct hdr* h = malloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->heap = heap;
    h->size = size;
    return h + 1;
}
void* heap_caps_malloc(size_t size, uint32_t caps) {
    for (int heap = HEAP_INTERNAL; heap <= HEAP_SPIRAM; heap++) {   // internal RAM first, as IDF orders 8-bit heaps
        if ((caps & caps_of(heap)) != caps) continue;
        void* p = block_in(heap, size);
        if (p) return p;
    }
    return NULL;
}
void heap_caps_free(void* p) { if (p) free((struct hdr*)p - 1); }
int stub_heap_of(const void* p) { return ((const struct hdr*)p - 1)->heap; }
void* heap_caps_realloc(void* p, size_t size, uint32_t caps) {
    const struct hdr* h = (const struct hdr*)p - 1;
    void* n = (caps & caps_of(h->heap)) == caps ? block_in(h->heap, size) : NULL;   // its own heap first
    if (!n) n = heap_caps_malloc(size, caps);
    if (!n) return NULL;
    memcpy(n, p, h->size < size ? h->size : size);
    heap_caps_free(p);
    return n;
}

static int g_fail = 0;
static void expect(const char* what, int ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

enum { SMALL = 64, BIG = 2048 };
#define TH CONFIG_LUA_ENGINE_INTERNAL_SMALL_THRESHOLD

int main(void) {
    void* s = lua_engine_alloc(NULL, NULL, 0, SMALL);
    expect(TH > SMALL ? "a small block starts in internal RAM" : "with threshold 0 a small block starts in PSRAM",
           s && stub_heap_of(s) == (TH > SMALL ? HEAP_INTERNAL : HEAP_SPIRAM));
    s = lua_engine_alloc(NULL, s, SMALL, 2 * SMALL);
    expect("a block that stays below the threshold keeps its heap",
           s && stub_heap_of(s) == (TH > 2 * SMALL ? HEAP_INTERNAL : HEAP_SPIRAM));
    s = lua_engine_alloc(NULL, s, 2 * SMALL, BIG);
    expect("a block that grows past the threshold ends in PSRAM", s && stub_heap_of(s) == HEAP_SPIRAM);

    void* b = lua_engine_alloc(NULL, NULL, 0, BIG);
    expect("a big block starts in PSRAM", b && stub_heap_of(b) == HEAP_SPIRAM);
    stub_spiram_full = 1;
    void* g = lua_engine_alloc(NULL, b, BIG, 2 * BIG);
    stub_spiram_full = 0;
    expect("a big block PSRAM cannot grow is refused, not moved to internal RAM", g == NULL);
    if (g) {   // it moved, the old pointer is gone
        lua_engine_alloc(NULL, g, 2 * BIG, 0);
        b = NULL;
    } else {
        expect("the refused block is untouched, in PSRAM", stub_heap_of(b) == HEAP_SPIRAM);
    }
    lua_engine_alloc(NULL, s, BIG, 0);
    if (b) lua_engine_alloc(NULL, b, BIG, 0);
    expect("the budget counter is back to 0", lua_engine_heap_used_bytes() == 0);
    return g_fail;
}
