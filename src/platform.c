#include "platform.h"

#include <stdarg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#include "arena.h"

void assert_fail(const char *file, int line, const char *expr)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
	fflush(stderr);
	abort();
}

void fatal(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("fatal: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	fflush(stderr);
	abort();
}

#ifdef _WIN32

uint8_t *platform_reserve(ptrdiff_t size)
{
	void *p = VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
	if (!p) fatal("cannot reserve %td bytes", size);
	return p;
}

void platform_commit(uint8_t *base, ptrdiff_t size)
{
	if (!VirtualAlloc(base, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE))
		fatal("cannot commit %td bytes", size);
}

void platform_release(uint8_t *base, ptrdiff_t size)
{
	(void)size;
	VirtualFree(base, 0, MEM_RELEASE);
}

ptrdiff_t platform_page_size(void)
{
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	return (ptrdiff_t)info.dwPageSize;
}

double platform_seconds(void)
{
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
}

#else

uint8_t *platform_reserve(ptrdiff_t size)
{
	void *p = mmap(nullptr, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) fatal("cannot reserve %td bytes", size);
	return p;
}

void platform_commit(uint8_t *base, ptrdiff_t size)
{
	if (mprotect(base, (size_t)size, PROT_READ | PROT_WRITE) != 0)
		fatal("cannot commit %td bytes", size);
}

void platform_release(uint8_t *base, ptrdiff_t size)
{
	munmap(base, (size_t)size);
}

ptrdiff_t platform_page_size(void)
{
	return (ptrdiff_t)sysconf(_SC_PAGESIZE);
}

double platform_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif

bool platform_read_file(Arena *arena, const char *path, Str *out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	bool ok = fseek(f, 0, SEEK_END) == 0;
	long size = ok ? ftell(f) : -1;
	ok = ok && size >= 0 && fseek(f, 0, SEEK_SET) == 0;
	char *buf = ok ? arena_try_push(arena, (ptrdiff_t)size + 1, 1) : nullptr;
	ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!ok) return false;
	*out = (Str){buf, (ptrdiff_t)size};
	return true;
}

bool platform_write_file(const char *path, const void *data, ptrdiff_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
	ok = (fclose(f) == 0) && ok;
	return ok;
}
