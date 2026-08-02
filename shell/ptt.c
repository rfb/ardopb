#include "shell/ptt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/net.h"
#include "shell/sys.h"

/**
 * @file ptt.c
 * @brief Keying the transmitter (see ptt.h).
 *
 * Talks to serial ports and to rigctld, so like the audio backends this is
 * exercised against hardware rather than in the in-process suite -- except the
 * rigctld path, which is tested against a scripted fake server on localhost.
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

struct ardop_ptt {
	ardop_ptt_config cfg;
	bool keyed;
	ardop_fault fault;
	char describe[ARDOP_PTT_TARGET_MAX + 32];

#ifdef _WIN32
	HANDLE serial;
#else
	int serial_fd;
#endif
	ardop_socket rig;   /* rigctld, or ARDOP_SOCKET_INVALID. */
};

/* ardop_fault_str lives in shell/fault.c: rendering a fault name should not
 * require linking a serial port and a rigctld socket. */

/* --- parsing --------------------------------------------------------------- */

static void set_target(ardop_ptt_config *out, const char *s)
{
#ifdef _WIN32
	/* "COM10" and above only open through the device-namespace form. The
	 * bare name works for COM1..COM9 and silently fails above it, which is
	 * exactly the case an operator with a couple of USB adapters hits. */
	if ((s[0] == 'C' || s[0] == 'c') && (s[1] == 'O' || s[1] == 'o')
	    && (s[2] == 'M' || s[2] == 'm') && s[3] != '\0') {
		snprintf(out->target, sizeof(out->target), "\\\\.\\%s", s);
		return;
	}
#endif
	snprintf(out->target, sizeof(out->target), "%s", s);
}

bool ardop_ptt_parse(const char *spec, ardop_ptt_config *out)
{
	memset(out, 0, sizeof(*out));

	if (!spec || !*spec || strcmp(spec, "none") == 0
	    || strcmp(spec, "vox") == 0) {
		out->method = ARDOP_PTT_NONE;
		return true;
	}

	if (strncmp(spec, "rts:", 4) == 0) {
		out->method = ARDOP_PTT_SERIAL_RTS;
		set_target(out, spec + 4);
		return true;
	}
	if (strncmp(spec, "dtr:", 4) == 0) {
		out->method = ARDOP_PTT_SERIAL_DTR;
		set_target(out, spec + 4);
		return true;
	}
	if (strncmp(spec, "rigctld:", 8) == 0) {
		const char *rest = spec + 8;
		const char *colon = strrchr(rest, ':');
		out->method = ARDOP_PTT_RIGCTLD;
		out->port = 4532;
		if (colon) {
			size_t hlen = (size_t)(colon - rest);
			if (hlen >= sizeof(out->target))
				hlen = sizeof(out->target) - 1;
			memcpy(out->target, rest, hlen);
			out->target[hlen] = '\0';
			out->port = (uint16_t)strtoul(colon + 1, NULL, 10);
		} else {
			snprintf(out->target, sizeof(out->target), "%s", rest);
		}
		if (out->target[0] == '\0')
			snprintf(out->target, sizeof(out->target), "127.0.0.1");
		return true;
	}

	if (strncmp(spec, "cm108:", 6) == 0) {
		fprintf(stderr, "ptt: CM108 HID keying is not implemented yet "
			"(it needs a HID dependency and the hardware to test "
			"against); use rts:, dtr: or rigctld:\n");
		return false;
	}
	if (strncmp(spec, "gpio:", 5) == 0) {
		fprintf(stderr, "ptt: GPIO keying is not implemented yet "
			"(it needs libgpiod); use rts:, dtr: or rigctld:\n");
		return false;
	}

	if (strchr(spec, ':') && strncmp(spec, "\\\\", 2) != 0) {
		fprintf(stderr, "ptt: unknown method in '%s'; expected none, "
			"rts:DEV, dtr:DEV or rigctld:HOST:PORT\n", spec);
		return false;
	}

	/* A bare path: what --ptt used to take. */
	out->method = ARDOP_PTT_SERIAL_RTS;
	set_target(out, spec);
	return true;
}

/* --- serial ---------------------------------------------------------------- */

static bool serial_open(ardop_ptt *p)
{
#ifdef _WIN32
	p->serial = CreateFileA(p->cfg.target, GENERIC_READ | GENERIC_WRITE, 0,
				NULL, OPEN_EXISTING, 0, NULL);
	if (p->serial == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "ptt: open %s failed (error %lu)\n",
			p->cfg.target, (unsigned long)GetLastError());
		return false;
	}
	/* Start unkeyed on both lines, whichever one we will drive. */
	EscapeCommFunction(p->serial, CLRRTS);
	EscapeCommFunction(p->serial, CLRDTR);
	return true;
#else
	p->serial_fd = open(p->cfg.target, O_RDWR | O_NOCTTY);
	if (p->serial_fd < 0) {
		perror("ptt: open");
		return false;
	}
	int bits = 0;
	if (ioctl(p->serial_fd, TIOCMGET, &bits) == 0) {
		bits &= ~(TIOCM_RTS | TIOCM_DTR);
		(void)ioctl(p->serial_fd, TIOCMSET, &bits);
	}
	return true;
#endif
}

static void serial_set(ardop_ptt *p, bool key)
{
#ifdef _WIN32
	if (p->serial == INVALID_HANDLE_VALUE)
		return;
	DWORD f;
	if (p->cfg.method == ARDOP_PTT_SERIAL_DTR)
		f = key ? SETDTR : CLRDTR;
	else
		f = key ? SETRTS : CLRRTS;
	if (!EscapeCommFunction(p->serial, f))
		p->fault = ARDOP_FAULT_PTT_LOST;
#else
	if (p->serial_fd < 0)
		return;
	int bits = 0;
	if (ioctl(p->serial_fd, TIOCMGET, &bits) != 0) {
		p->fault = ARDOP_FAULT_PTT_LOST;
		return;
	}
	int line = (p->cfg.method == ARDOP_PTT_SERIAL_DTR) ? TIOCM_DTR
							   : TIOCM_RTS;
	if (key)
		bits |= line;
	else
		bits &= ~line;
	if (ioctl(p->serial_fd, TIOCMSET, &bits) != 0)
		p->fault = ARDOP_FAULT_PTT_LOST;
#endif
}

/* --- rigctld --------------------------------------------------------------- */

/*
 * Over TCP to a running rigctld, deliberately not by linking hamlib: no link
 * dependency, no LGPL entanglement, no exposure to hamlib API churn, and most
 * operators already have rigctld running.
 */
static void rig_set(ardop_ptt *p, bool key)
{
	if (!ardop_net_valid(p->rig)) {
		p->fault = ARDOP_FAULT_PTT_LOST;
		return;
	}

	char cmd[8];
	int n = snprintf(cmd, sizeof(cmd), "T %d\n", key ? 1 : 0);
	size_t off = 0;
	while (off < (size_t)n) {
		size_t moved = 0;
		ardop_net_status st = ardop_net_send(p->rig, cmd + off,
						     (size_t)n - off, &moved);
		if (st == ARDOP_NET_AGAIN)
			continue;
		if (st != ARDOP_NET_OK) {
			fprintf(stderr, "ptt: rigctld write failed: %s\n",
				ardop_net_last_error());
			p->fault = ARDOP_FAULT_PTT_LOST;
			return;
		}
		off += moved;
	}

	/* rigctld answers "RPRT 0" on success. Waiting for it is what turns a
	 * dead CAT link into a fault instead of a silent no-transmission; the
	 * modem thread can afford the round trip, for the same reason it can
	 * afford to block draining the playback ring. */
	uint64_t deadline = ardop_mono_ms() + 200;
	char reply[64];
	size_t len = 0;
	for (;;) {
		size_t got = 0;
		ardop_net_status st = ardop_net_recv(p->rig, reply + len,
						     sizeof(reply) - 1 - len,
						     &got);
		if (st == ARDOP_NET_OK) {
			len += got;
			reply[len] = '\0';
			if (strchr(reply, '\n'))
				break;
		} else if (st == ARDOP_NET_AGAIN) {
			if (ardop_mono_ms() > deadline) {
				fprintf(stderr, "ptt: rigctld did not answer "
					"within 200 ms\n");
				p->fault = ARDOP_FAULT_PTT_LOST;
				return;
			}
			ardop_sleep_ms(2);
		} else {
			fprintf(stderr, "ptt: rigctld closed the connection\n");
			p->fault = ARDOP_FAULT_PTT_LOST;
			return;
		}
		if (len >= sizeof(reply) - 1)
			break;
	}

	if (strncmp(reply, "RPRT 0", 6) != 0) {
		fprintf(stderr, "ptt: rigctld refused: %s", reply);
		p->fault = ARDOP_FAULT_PTT_LOST;
	}
}

/* --- lifecycle ------------------------------------------------------------- */

ardop_ptt *ardop_ptt_open(const ardop_ptt_config *cfg)
{
	ardop_ptt *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->cfg = *cfg;
	p->rig = ARDOP_SOCKET_INVALID;
#ifdef _WIN32
	p->serial = INVALID_HANDLE_VALUE;
#else
	p->serial_fd = -1;
#endif

	switch (cfg->method) {
	case ARDOP_PTT_NONE:
		snprintf(p->describe, sizeof(p->describe), "none (VOX)");
		return p;

	case ARDOP_PTT_SERIAL_RTS:
	case ARDOP_PTT_SERIAL_DTR:
		if (!serial_open(p)) {
			free(p);
			return NULL;
		}
		snprintf(p->describe, sizeof(p->describe), "%s:%s",
			 cfg->method == ARDOP_PTT_SERIAL_DTR ? "dtr" : "rts",
			 cfg->target);
		return p;

	case ARDOP_PTT_RIGCTLD:
		p->rig = ardop_net_connect(cfg->target, cfg->port);
		if (!ardop_net_valid(p->rig)) {
			fprintf(stderr, "ptt: cannot reach rigctld at %s:%u\n",
				cfg->target, (unsigned)cfg->port);
			free(p);
			return NULL;
		}
		(void)ardop_net_set_nonblock(p->rig);
		snprintf(p->describe, sizeof(p->describe), "rigctld:%s:%u",
			 cfg->target, (unsigned)cfg->port);
		/* Prove the link now rather than at the first transmission. */
		rig_set(p, false);
		if (p->fault != ARDOP_FAULT_NONE) {
			ardop_net_close(&p->rig);
			free(p);
			return NULL;
		}
		return p;

	case ARDOP_PTT_CM108:
	case ARDOP_PTT_GPIO:
		break;
	}

	fprintf(stderr, "ptt: method not implemented\n");
	free(p);
	return NULL;
}

void ardop_ptt_set(ardop_ptt *p, bool key)
{
	if (!p)
		return;
	p->keyed = key;

	switch (p->cfg.method) {
	case ARDOP_PTT_NONE:
		break;
	case ARDOP_PTT_SERIAL_RTS:
	case ARDOP_PTT_SERIAL_DTR:
		serial_set(p, key);
		break;
	case ARDOP_PTT_RIGCTLD:
		rig_set(p, key);
		break;
	case ARDOP_PTT_CM108:
	case ARDOP_PTT_GPIO:
		break;
	}
}

bool ardop_ptt_keyed(const ardop_ptt *p)
{
	return p && p->keyed;
}

ardop_fault ardop_ptt_fault(const ardop_ptt *p)
{
	return p ? p->fault : ARDOP_FAULT_NONE;
}

const char *ardop_ptt_describe(const ardop_ptt *p)
{
	return p ? p->describe : "none";
}

void ardop_ptt_close(ardop_ptt *p)
{
	if (!p)
		return;

	/* Unkey before releasing anything. Order matters: a serial line falls
	 * when the handle closes, but a rig keyed over rigctld stays keyed
	 * forever if we just drop the socket. */
	if (p->keyed) {
		p->fault = ARDOP_FAULT_NONE;   /* give the unkey a clean try. */
		ardop_ptt_set(p, false);
	}

#ifdef _WIN32
	if (p->serial != INVALID_HANDLE_VALUE)
		CloseHandle(p->serial);
#else
	if (p->serial_fd >= 0)
		close(p->serial_fd);
#endif
	ardop_net_close(&p->rig);
	free(p);
}
