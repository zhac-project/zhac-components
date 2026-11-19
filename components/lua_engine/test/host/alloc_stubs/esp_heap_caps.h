// Host stand-in for esp_heap_caps.h, modelling the IDF heap choice lua_alloc.c relies on: two heaps, internal RAM
// first for a request either may serve; heap_caps_realloc keeps a block in its own heap when that heap has every
// asked cap and room (multi_heap_realloc), and otherwise allocates in the first heap that has them.
#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
enum { HEAP_INTERNAL = 1, HEAP_SPIRAM = 2 };
void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
void  heap_caps_free(void* ptr);
int   stub_heap_of(const void* ptr);   // HEAP_INTERNAL or HEAP_SPIRAM
extern int stub_spiram_full;           // nonzero: PSRAM has no room left
