#include "platform.h"

#include <stdarg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
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

bool platform_write_file_atomic(const char *path, const void *data, ptrdiff_t len)
{
	char tmp[4096];
	int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (n <= 0 || n >= (int)sizeof tmp) return false;
	FILE *f = fopen(tmp, "wb");
	if (!f) return false;
	bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len && fflush(f) == 0;
#ifndef _WIN32
	ok = ok && fsync(fileno(f)) == 0;
#endif
	ok = (fclose(f) == 0) && ok;
	return ok && rename(tmp, path) == 0;
}

bool platform_write_file(const char *path, const void *data, ptrdiff_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
	ok = (fclose(f) == 0) && ok;
	return ok;
}

bool platform_file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f) fclose(f);
	return f != nullptr;
}

#ifdef _WIN32

bool platform_absolute_path(const char *path, char *buf, ptrdiff_t size)
{
	DWORD n = GetFullPathNameA(path, (DWORD)size, buf, nullptr);
	return n > 0 && n < (DWORD)size;
}

bool platform_make_dir(const char *path) { return CreateDirectoryA(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS; }
bool platform_clipboard_copy_file(const char *path) { (void)path; return false; }
bool platform_spawn(const char *const argv[]) { (void)argv; return false; }
int platform_run(const char *const argv[]) { (void)argv; return -1; }
void platform_reap_children(void) {}
int platform_spawn_detached(const char *const argv[], const char *log_path) { (void)argv, (void)log_path; return 0; }
bool platform_alive(int pid) { (void)pid; return false; }
bool platform_terminate(int pid) { (void)pid; return false; }
ptrdiff_t platform_stdin_read(char *buf, ptrdiff_t cap) { (void)buf, (void)cap; return -1; }

#else

ptrdiff_t platform_stdin_read(char *buf, ptrdiff_t cap)
{
	static bool nonblocking;
	if (!nonblocking) {
		fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
		nonblocking = true;
	}
	ssize_t n = read(0, buf, (size_t)cap);
	if (n > 0) return (ptrdiff_t)n;
	return n == 0 ? -1 : 0; // 0 bytes read: closed; would block: nothing yet
}

static bool spawn_child(const char *const argv[], pid_t *pid)
{
	// posix_spawnp's argv isn't const-qualified, but it doesn't write to it.
	return posix_spawnp(pid, argv[0], nullptr, nullptr, (char *const *)argv, environ) == 0;
}

bool platform_spawn(const char *const argv[])
{
	pid_t pid;
	return spawn_child(argv, &pid);
}

int platform_spawn_detached(const char *const argv[], const char *log_path)
{
	posix_spawnattr_t attr;
	posix_spawn_file_actions_t files;
	if (posix_spawnattr_init(&attr) != 0) return 0;
	if (posix_spawn_file_actions_init(&files) != 0) {
		posix_spawnattr_destroy(&attr);
		return 0;
	}
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&files, 1, log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_adddup2(&files, 1, 2);
	pid_t pid = 0;
	bool ok = posix_spawnp(&pid, argv[0], &files, &attr, (char *const *)argv, environ) == 0;
	posix_spawn_file_actions_destroy(&files);
	posix_spawnattr_destroy(&attr);
	return ok ? (int)pid : 0;
}

bool platform_alive(int pid)
{
	if (pid <= 0) return false;
	waitpid(pid, nullptr, WNOHANG); // reap it if it was ours and has exited
	return kill(pid, 0) == 0;
}

bool platform_terminate(int pid) { return pid > 0 && kill(pid, SIGTERM) == 0; }

int platform_run(const char *const argv[])
{
	pid_t pid;
	if (!spawn_child(argv, &pid)) return -1;
	int status;
	if (waitpid(pid, &status, 0) != pid) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool platform_absolute_path(const char *path, char *buf, ptrdiff_t size)
{
	if (size < 4096) return false; // realpath writes up to PATH_MAX
	return realpath(path, buf) != nullptr;
}

bool platform_make_dir(const char *path)
{
	struct stat st;
	return mkdir(path, 0755) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

void platform_reap_children(void)
{
	while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

#ifdef __APPLE__

bool platform_clipboard_copy_file(const char *path)
{
	char script[4096];
	int n = snprintf(script, sizeof script, "set the clipboard to (POSIX file \"%s\")", path);
	if (n < 0 || n >= (int)sizeof script || strchr(path, '"')) return false;
	const char *argv[] = {"osascript", "-e", script, nullptr};
	return platform_run(argv) == 0;
}

#else

// X11 via xclip, as a file URI list: what file managers and chat apps paste.
bool platform_clipboard_copy_file(const char *path)
{
	FILE *p = popen("xclip -selection clipboard -t text/uri-list", "w");
	if (!p) return false;
	fprintf(p, "file://%s\n", path);
	return pclose(p) == 0;
}

#endif
#endif
