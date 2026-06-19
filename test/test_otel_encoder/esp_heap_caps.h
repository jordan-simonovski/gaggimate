// Host shim for the ESP heap-caps API. On the device the encoder prefers PSRAM;
// on the host every allocation is a plain malloc. Only the calls OtlpEncoder
// makes are provided.
#pragma once

#include <cstddef>
#include <cstdlib>

#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0

static inline void *heap_caps_malloc(size_t size, int /*caps*/) { return malloc(size); }
static inline void heap_caps_free(void *ptr) { free(ptr); }
static inline void *heap_caps_realloc(void *ptr, size_t size, int /*caps*/) { return realloc(ptr, size); }
