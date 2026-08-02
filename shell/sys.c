#include "shell/sys.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/**
 * @file sys.c
 * @brief The non-socket platform surface (see sys.h).
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#else
#  include <sys/select.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#endif

#include <stdlib.h>

/* --- where things are kept ------------------------------------------------- */

bool ardop_mkdir_p(const char *path)
{
	char buf[512];
	size_t n = strlen(path);
	if (n == 0 || n >= sizeof(buf))
		return false;
	memcpy(buf, path, n + 1);

	/* Create each component in turn. Starting at 1 skips a leading separator
	 * so an absolute path does not try to create the root. */
	for (size_t i = 1; i <= n; i++) {
		bool sep = buf[i] == '/' || buf[i] == '\\';
		if (!sep && buf[i] != '\0')
			continue;

		char saved = buf[i];
		buf[i] = '\0';
#ifdef _WIN32
		/* "C:" is a drive, not a directory to create. */
		bool drive = i == 2 && buf[1] == ':';
		if (!drive && !CreateDirectoryA(buf, NULL)
		    && GetLastError() != ERROR_ALREADY_EXISTS) {
			buf[i] = saved;
			return false;
		}
#else
		if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
			buf[i] = saved;
			return false;
		}
#endif
		buf[i] = saved;
	}
	return true;
}

bool ardop_config_dir(const char *app, char *out, size_t cap)
{
	if (cap == 0)
		return false;
	out[0] = '\0';

#ifdef _WIN32
	const char *base = getenv("APPDATA");
	if (!base || !*base)
		return false;
	if ((size_t)snprintf(out, cap, "%s\\%s", base, app) >= cap) {
		out[0] = '\0';
		return false;
	}
#else
	const char *xdg = getenv("XDG_CONFIG_HOME");
	int n;
	if (xdg && *xdg) {
		n = snprintf(out, cap, "%s/%s", xdg, app);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home)
			return false;
		n = snprintf(out, cap, "%s/.config/%s", home, app);
	}
	if (n < 0 || (size_t)n >= cap) {
		out[0] = '\0';
		return false;
	}
#endif

	if (!ardop_mkdir_p(out)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool ardop_replace_file(const char *from, const char *to)
{
#ifdef _WIN32
	/* MoveFileEx with REPLACE_EXISTING is the atomic-replace primitive;
	 * plain rename() fails outright when the destination exists. */
	return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return rename(from, to) == 0;
#endif
}

/* --- sleeping and clocks --------------------------------------------------- */

void ardop_sleep_ms(unsigned ms)
{
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
#endif
}

uint64_t ardop_mono_ms(void)
{
#ifdef _WIN32
	/* QueryPerformanceCounter rather than GetTickCount64: the drain deadline
	 * around PTT is measured in tens of milliseconds and GetTickCount's
	 * resolution is up to ~16 ms. */
	static LARGE_INTEGER freq;
	if (freq.QuadPart == 0)
		QueryPerformanceFrequency(&freq);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (uint64_t)((now.QuadPart * 1000) / freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

uint64_t ardop_wall_ms(void)
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER u;
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	/* FILETIME is 100 ns ticks since 1601-01-01. */
	return (uint64_t)(u.QuadPart / 10000ULL) - 11644473600000ULL;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

/* --- stdio ----------------------------------------------------------------- */

void ardop_stdio_binary(void)
{
#ifdef _WIN32
	(void)_setmode(_fileno(stdin), _O_BINARY);
	(void)_setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool ardop_stdin_ready(int timeout_ms)
{
#ifdef _WIN32
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	if (h == INVALID_HANDLE_VALUE || h == NULL)
		return false;

	DWORD type = GetFileType(h);
	if (type == FILE_TYPE_DISK)
		return true;   /* a redirected file is always readable. */

	if (type == FILE_TYPE_PIPE) {
		/* PeekNamedPipe does not block and does not consume. Poll it,
		 * because a pipe handle is not waitable for "has data". */
		DWORD waited = 0;
		for (;;) {
			DWORD avail = 0;
			if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
				return true;   /* broken: let the read report it. */
			if (avail > 0)
				return true;
			if (timeout_ms >= 0 && waited >= (DWORD)timeout_ms)
				return false;
			Sleep(5);
			waited += 5;
		}
	}

	/* A console. Waiting on the handle wakes for key-up, focus and mouse
	 * events too, so filter to actual key presses or the caller spins. */
	DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
	for (;;) {
		if (WaitForSingleObject(h, ms) != WAIT_OBJECT_0)
			return false;

		INPUT_RECORD rec[16];
		DWORD got = 0;
		if (!PeekConsoleInput(h, rec, 16, &got) || got == 0)
			return false;
		for (DWORD i = 0; i < got; i++)
			if (rec[i].EventType == KEY_EVENT
			    && rec[i].Event.KeyEvent.bKeyDown)
				return true;
		/* Only noise was pending: drain it and go round again. */
		ReadConsoleInput(h, rec, got, &got);
		if (ms == 0)
			return false;
	}
#else
	fd_set r;
	FD_ZERO(&r);
	FD_SET(0, &r);
	struct timeval tv;
	struct timeval *tvp = NULL;
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		tvp = &tv;
	}
	int s = select(1, &r, NULL, NULL, tvp);
	return s > 0 && FD_ISSET(0, &r);
#endif
}

/* --- interrupt handling ---------------------------------------------------- */

static volatile sig_atomic_t stop_flag;

#ifdef _WIN32
static BOOL WINAPI console_ctrl(DWORD type)
{
	(void)type;
	stop_flag = 1;
	return TRUE;
}
#else
static void on_signal(int sig)
{
	(void)sig;
	stop_flag = 1;
}
#endif

void ardop_install_signal_handlers(void)
{
#ifdef _WIN32
	SetConsoleCtrlHandler(console_ctrl, TRUE);
#else
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	/* A host that vanishes mid-write must not take the modem with it; the
	 * send path reports the error instead. */
	signal(SIGPIPE, SIG_IGN);
#endif
}

bool ardop_stop_requested(void)
{
	return stop_flag != 0;
}

/* --- the timed binary semaphore -------------------------------------------- */

#ifdef _WIN32
typedef struct {
	HANDLE ev;
} sig_impl;
#else
typedef struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool posted;
} sig_impl;
#endif

_Static_assert(sizeof(sig_impl) <= sizeof(((ardop_signal *)0)->opaque),
	       "ardop_signal.opaque too small for this platform");

bool ardop_signal_init(ardop_signal *sig)
{
	memset(sig, 0, sizeof(*sig));
	sig_impl *s = (sig_impl *)(void *)sig->opaque;
#ifdef _WIN32
	/* Auto-reset, initially unsignalled. */
	s->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!s->ev)
		return false;
#else
	if (pthread_mutex_init(&s->mu, NULL) != 0)
		return false;
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	/* Wait against the monotonic clock, so a system time step (ntp, a
	 * daylight-saving change) cannot stall a drain deadline. */
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	int rc = pthread_cond_init(&s->cv, &attr);
	pthread_condattr_destroy(&attr);
	if (rc != 0) {
		pthread_mutex_destroy(&s->mu);
		return false;
	}
	s->posted = false;
#endif
	sig->ready = true;
	return true;
}

void ardop_signal_post(ardop_signal *sig)
{
	if (!sig->ready)
		return;
	sig_impl *s = (sig_impl *)(void *)sig->opaque;
#ifdef _WIN32
	SetEvent(s->ev);
#else
	pthread_mutex_lock(&s->mu);
	s->posted = true;
	pthread_cond_signal(&s->cv);
	pthread_mutex_unlock(&s->mu);
#endif
}

bool ardop_signal_wait(ardop_signal *sig, unsigned timeout_ms)
{
	if (!sig->ready)
		return false;
	sig_impl *s = (sig_impl *)(void *)sig->opaque;
#ifdef _WIN32
	return WaitForSingleObject(s->ev, (DWORD)timeout_ms) == WAIT_OBJECT_0;
#else
	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += (time_t)(timeout_ms / 1000u);
	deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&s->mu);
	int rc = 0;
	while (!s->posted && rc == 0)
		rc = pthread_cond_timedwait(&s->cv, &s->mu, &deadline);
	bool got = s->posted;
	s->posted = false;
	pthread_mutex_unlock(&s->mu);
	return got;
#endif
}

void ardop_signal_destroy(ardop_signal *sig)
{
	if (!sig->ready)
		return;
	sig_impl *s = (sig_impl *)(void *)sig->opaque;
#ifdef _WIN32
	CloseHandle(s->ev);
#else
	pthread_cond_destroy(&s->cv);
	pthread_mutex_destroy(&s->mu);
#endif
	sig->ready = false;
}
