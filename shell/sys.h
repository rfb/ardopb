#ifndef ARDOP_SHELL_SYS_H_
#define ARDOP_SHELL_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file sys.h
 * @brief The non-socket platform surface: sleeping, clocks, stdio mode, signals.
 *
 * Like [net.h](net.h) this includes no operating-system header, so nothing
 * above it inherits `<windows.h>`.
 */

/** @brief Sleep for at least @p ms milliseconds. */
void ardop_sleep_ms(unsigned ms);

/**
 * @brief Monotonic milliseconds since an arbitrary epoch.
 *
 * For measuring elapsed intervals -- timeouts, drain deadlines. Never a
 * protocol deadline: those are sample-counted in the core (analysis/06 Rule 2).
 */
uint64_t ardop_mono_ms(void);

/**
 * @brief Wall-clock milliseconds since the Unix epoch.
 *
 * For human timestamps and the regulatory 10-minute identification interval,
 * which is a legal obligation rather than a protocol deadline.
 */
uint64_t ardop_wall_ms(void);

/**
 * @brief Put stdin and stdout into binary mode.
 *
 * A no-op on POSIX and **mandatory** on Windows for anything that pipes data.
 * In the default text mode the C runtime turns every 0x0A written into 0x0D
 * 0x0A, and treats 0x1A on input as end-of-file. `ardop-tx` reads payload from
 * stdin and `ardop-rx` writes payload to stdout, so without this a file moved
 * over the radio is silently corrupted at both ends -- and the corruption
 * depends on the file's contents, so small tests pass.
 */
void ardop_stdio_binary(void);

/**
 * @brief True if stdin has input ready within @p timeout_ms.
 *
 * Exists because Winsock's `select()` accepts sockets only -- a console handle,
 * a pipe and a file are all invalid there -- while `apps/ardop_chat.c` must
 * watch the keyboard and two sockets at once. Callers poll this next to
 * ::ardop_net_wait rather than selecting over a mixed set.
 *
 * @param timeout_ms 0 to poll without blocking.
 */
bool ardop_stdin_ready(int timeout_ms);

/**
 * @brief Install handlers so an interrupt sets ::ardop_stop_requested.
 *
 * Matters more than usual here: PTT keyed over a CAT link (rigctld) does *not*
 * drop when the process dies, because the rig never learns the controller is
 * gone. A serial RTS line falls with the handle; a TCP command does not. So the
 * program has to unkey on the way out rather than rely on the OS.
 */
void ardop_install_signal_handlers(void);

/** @brief True once an interrupt has been received. */
bool ardop_stop_requested(void);

/**
 * @brief A binary semaphore with a timed wait.
 *
 * The handshake between an OS audio callback and the modem thread: the callback
 * posts after each device period, the modem thread waits for samples rather
 * than spinning. ::ardop_signal_post is safe to call from a real-time audio
 * callback; it neither allocates nor blocks.
 *
 * Opaque and heap-free: the storage is sized here so a backend can embed one.
 */
typedef struct {
	/* Large enough for a Windows HANDLE or a pthread mutex + condvar. */
	_Alignas(16) unsigned char opaque[128];
	bool ready;
} ardop_signal;

/** @brief Initialise @p sig. @return false on failure. */
bool ardop_signal_init(ardop_signal *sig);

/** @brief Wake one waiter. Safe from an audio callback. */
void ardop_signal_post(ardop_signal *sig);

/**
 * @brief Wait for a post.
 * @return true if posted, false if @p timeout_ms elapsed first.
 */
bool ardop_signal_wait(ardop_signal *sig, unsigned timeout_ms);

/** @brief Release @p sig. */
void ardop_signal_destroy(ardop_signal *sig);

#endif /* ARDOP_SHELL_SYS_H_ */
