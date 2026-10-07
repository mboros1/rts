// The edge with the operating system: virtual memory, time, files.
// The only module that talks to the OS; nothing calls malloc.
#pragma once

#include "base.h"

// Reserve a range of address space with no memory behind it yet.
// Aborts if the OS refuses: reservations happen at startup.
uint8_t *platform_reserve(ptrdiff_t size);

// Make [base, base + size) usable. Both must be page aligned.
void platform_commit(uint8_t *base, ptrdiff_t size);

void platform_release(uint8_t *base, ptrdiff_t size);

ptrdiff_t platform_page_size(void);

// Monotonic seconds, for profiling only. The sim never reads the clock.
double platform_seconds(void);

// Read a whole file into memory from the arena. Returns false if it cannot.
typedef struct Arena Arena;
bool platform_read_file(Arena *arena, const char *path, Str *out);

bool platform_write_file(const char *path, const void *data, ptrdiff_t len);
