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

// Write to PATH.tmp, fsync, rename over PATH: after a crash the file is the
// old version or the new one, never half of either.
bool platform_write_file_atomic(const char *path, const void *data, ptrdiff_t len);

// Put a file on the system clipboard, so pasting into a chat or a folder
// attaches the file. path must be absolute. False if the OS tool failed or
// this platform has no implementation yet.
bool platform_clipboard_copy_file(const char *path);

// Start a program and don't wait for it. argv ends with nullptr.
bool platform_spawn(const char *const argv[]);

// Run a program and wait for it. Returns its exit status, or -1.
int platform_run(const char *const argv[]);

// Collect background children that have exited. Call once a frame.
void platform_reap_children(void);

bool platform_make_dir(const char *path);   // true if it exists afterwards
bool platform_file_exists(const char *path);

// Absolute form of a path to an existing file, into buf. False on failure.
bool platform_absolute_path(const char *path, char *buf, ptrdiff_t size);
