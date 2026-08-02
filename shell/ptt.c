#include "shell/ptt.h"

#include "shell/ptt_cat.h"
#include "shell/ptt_cm108.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/net.h"
#include "shell/sys.h"

/**
 * @file ptt.c
 * @brief Keying the transmitter (see ptt.h).
 *
 * Talks to serial ports, to rigctld and to raw HID, so the device half is
 * exercised against hardware rather than in the in-process suite.
 *
 * What *is* covered by `test/core/test_ptt.c`, with no hardware: the whole
 * specification grammar, the CAT frames and their replies byte for byte, the
 * CM108 report and chip table, and the auto-selection policy. What is not: the
 * rigctld byte exchange, which needs a server answering while open() blocks and
 * therefore a second thread; and every write to an actual device. Those are
 * named in that file rather than assumed.
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
	ardop_cm108 *hid;   /* CM108, or NULL. */
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
		out->method = ARDOP_PTT_RIGCTLD;
		out->port = 4532;

		/* An IPv6 literal is full of colons, so splitting on the last
		 * one turns "rigctld:::1" into host ":" port 1. Bracketed form
		 * first: [::1] or [::1]:4532. */
		if (*rest == '[') {
			const char *close = strchr(rest, ']');
			if (!close) {
				fprintf(stderr, "ptt: unterminated '[' in "
					"'%s'\n", spec);
				return false;
			}
			size_t hlen = (size_t)(close - rest - 1);
			if (hlen >= sizeof(out->target))
				hlen = sizeof(out->target) - 1;
			memcpy(out->target, rest + 1, hlen);
			out->target[hlen] = '\0';
			if (close[1] == ':')
				out->port = (uint16_t)strtoul(close + 2, NULL, 10);
			if (out->target[0] == '\0')
				snprintf(out->target, sizeof(out->target),
					 "127.0.0.1");
			return true;
		}

		const char *colon = strrchr(rest, ':');
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

	if (strncmp(spec, "cm108", 5) == 0 &&
	    (spec[5] == '\0' || spec[5] == ':' || spec[5] == '+')) {
		out->method = ARDOP_PTT_CM108;
		out->gpio = ARDOP_CM108_DEFAULT_GPIO;

		char rest[ARDOP_PTT_TARGET_MAX];
		snprintf(rest, sizeof rest, "%s", spec[5] == ':' ? spec + 6
							         : spec + 5);

		/* "+N" selects the GPIO pin, and may follow any of the forms. */
		char *plus = strrchr(rest, '+');
		if (plus) {
			*plus = '\0';
			long g = strtol(plus + 1, NULL, 10);
			if (g < 1 || g > 8) {
				fprintf(stderr, "ptt: cm108 GPIO pin must be "
					"1..8, not %ld\n", g);
				return false;
			}
			out->gpio = (unsigned)g;
		}

		/* "VID:PID" narrows an otherwise ambiguous scan. */
		unsigned v = 0, d = 0;
		if (strlen(rest) == 9 && rest[4] == ':' &&
		    sscanf(rest, "%4x:%4x", &v, &d) == 2) {
			out->hid_vid = (uint16_t)v;
			out->hid_pid = (uint16_t)d;
			return true;
		}

		if (*rest && strcmp(rest, "auto") != 0)
			snprintf(out->target, sizeof out->target, "%s", rest);
		return true;
	}
	{
		static const char *const kCat[] = {"civ:", "icom:", "xiegu:",
						   "kenwood:", "yaesu:"};
		for (size_t i = 0; i < sizeof kCat / sizeof kCat[0]; i++) {
			size_t n = strlen(kCat[i]);
			if (strncmp(spec, kCat[i], n) != 0)
				continue;

			char word[16];
			snprintf(word, sizeof word, "%.*s", (int)(n - 1), kCat[i]);
			ardop_cat_family fam;
			(void)ardop_cat_family_from_str(word, &fam);

			out->method = ARDOP_PTT_CAT;
			out->cat_family = (int)fam;
			out->civ_addr = ARDOP_CIV_DEFAULT_ADDR;

			char rest[ARDOP_PTT_TARGET_MAX];
			snprintf(rest, sizeof rest, "%s", spec + n);

			/* "@a4" is the CI-V transceiver address, in hex. It is
			 * a property of the radio -- a4 for the Xiegu X6100,
			 * X6200 and G90, 94 for an IC-7300 -- so the radio table
			 * carries it and an operator never has to know. */
			/* Trailing modifiers, innermost first: the address is
			 * written after the rate ("PORT:38400@a4"), so stripping
			 * the rate first would take the address with it. */
			char *at = strrchr(rest, '@');
			if (at) {
				*at = '\0';
				unsigned a = 0;
				if (sscanf(at + 1, "%2x", &a) != 1 || a > 0xFF) {
					fprintf(stderr, "ptt: bad CI-V address "
						"'%s'\n", at + 1);
					return false;
				}
				out->civ_addr = (uint8_t)a;
			}

			/* ":19200" overrides the baud rate. A radio set to
			 * something else simply does not answer, which looks
			 * exactly like a dead cable. */
			char *baud = strrchr(rest, ':');
			if (baud) {
				long b = strtol(baud + 1, NULL, 10);
				if (b >= 1200 && b <= 115200) {
					*baud = '\0';
					out->baud = (unsigned)b;
				}
			}

			if (!*rest) {
				fprintf(stderr, "ptt: %s needs a serial port, "
					"e.g. %s/dev/ttyUSB0\n", word, kCat[i]);
				return false;
			}
			set_target(out, rest);
			return true;
		}
	}

	if (strncmp(spec, "gpio:", 5) == 0) {
		fprintf(stderr, "ptt: GPIO keying is not implemented yet "
			"(it needs libgpiod); use rts:, dtr: or rigctld:\n");
		return false;
	}

	if (strchr(spec, ':') && strncmp(spec, "\\\\", 2) != 0) {
		fprintf(stderr, "ptt: unknown method in '%s'; expected none, "
			"rts:DEV, dtr:DEV, civ:DEV, kenwood:DEV, yaesu:DEV, "
			"cm108[:PATH] or rigctld:HOST:PORT\n", spec);
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

/* --- serial byte I/O, for the CAT methods ---------------------------------- */

/*
 * RTS and DTR keying never touches the data lines, so serial_open() has never
 * configured a baud rate. CAT does, and gets it wrong silently: at the wrong
 * rate the radio simply does not answer, which looks exactly like a dead cable.
 *
 * 19200 is the default because it is what the Xiegu family runs at (the G90 is
 * fixed there) and a common Icom setting. A radio set to something else needs
 * the rate given explicitly.
 */
static bool serial_configure(ardop_ptt *p, unsigned baud)
{
#ifdef _WIN32
	DCB dcb;
	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(p->serial, &dcb))
		return false;
	dcb.BaudRate = baud;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	if (!SetCommState(p->serial, &dcb))
		return false;

	COMMTIMEOUTS to;
	memset(&to, 0, sizeof to);
	to.ReadIntervalTimeout = MAXDWORD;   /* return immediately */
	return SetCommTimeouts(p->serial, &to) != 0;
#else
	struct termios t;
	if (tcgetattr(p->serial_fd, &t) != 0)
		return false;

	speed_t sp;
	switch (baud) {
	case 4800:   sp = B4800;   break;
	case 9600:   sp = B9600;   break;
	case 19200:  sp = B19200;  break;
	case 38400:  sp = B38400;  break;
	case 57600:  sp = B57600;  break;
	case 115200: sp = B115200; break;
	default:     sp = B19200;  break;
	}
	cfmakeraw(&t);
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~CRTSCTS;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;   /* non-blocking reads; cat_set does the waiting */
	return tcsetattr(p->serial_fd, TCSANOW, &t) == 0;
#endif
}

static bool serial_write(ardop_ptt *p, const uint8_t *buf, size_t n)
{
#ifdef _WIN32
	DWORD wrote = 0;
	return WriteFile(p->serial, buf, (DWORD)n, &wrote, NULL) && wrote == n;
#else
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(p->serial_fd, buf + off, n - off);
		if (w <= 0)
			return false;
		off += (size_t)w;
	}
	return true;
#endif
}

static size_t serial_read(ardop_ptt *p, uint8_t *buf, size_t cap)
{
	if (cap == 0)
		return 0;
#ifdef _WIN32
	DWORD got = 0;
	if (!ReadFile(p->serial, buf, (DWORD)cap, &got, NULL))
		return 0;
	return got;
#else
	ssize_t r = read(p->serial_fd, buf, cap);
	return r > 0 ? (size_t)r : 0;
#endif
}

static void serial_close(ardop_ptt *p)
{
#ifdef _WIN32
	if (p->serial != INVALID_HANDLE_VALUE)
		CloseHandle(p->serial);
	p->serial = INVALID_HANDLE_VALUE;
#else
	if (p->serial_fd >= 0)
		close(p->serial_fd);
	p->serial_fd = -1;
#endif
}

/* --- native CAT ------------------------------------------------------------ */

/*
 * The radio's own keying command, down its own serial port.
 *
 * The reply is waited for, on the same 200 ms budget the rigctld path uses and
 * for the same reason: a CAT link that has died must become a fault rather than
 * a silent no-transmission. Kenwood answers nothing, so there is nothing to wait
 * for and ardop_cat_is_acknowledged says so rather than treating silence as
 * success.
 */
static void cat_set(ardop_ptt *p, bool key)
{
	uint8_t frame[ARDOP_CAT_FRAME_MAX];
	ardop_cat_family fam = (ardop_cat_family)p->cfg.cat_family;
	size_t n = ardop_cat_frame(fam, p->cfg.civ_addr, key, frame,
				   sizeof frame);
	if (n == 0) {
		p->fault = ARDOP_FAULT_PTT_LOST;
		return;
	}

	if (!serial_write(p, frame, n)) {
		fprintf(stderr, "ptt: %s write failed\n",
			ardop_cat_family_str(fam));
		p->fault = ARDOP_FAULT_PTT_LOST;
		return;
	}

	if (!ardop_cat_is_acknowledged(fam))
		return;

	uint64_t deadline = ardop_mono_ms() + 200;
	uint8_t buf[64];
	size_t len = 0;
	for (;;) {
		size_t got = serial_read(p, buf + len, sizeof buf - len);
		if (got > 0) {
			len += got;
			size_t consumed = 0;
			ardop_cat_reply r = ardop_cat_parse_reply(
				fam, p->cfg.civ_addr, buf, len, &consumed);
			if (r == ARDOP_CAT_ACK)
				return;
			if (r == ARDOP_CAT_NAK) {
				fprintf(stderr, "ptt: the radio refused the "
					"%s keying command\n",
					ardop_cat_family_str(fam));
				p->fault = ARDOP_FAULT_PTT_LOST;
				return;
			}
			if (r == ARDOP_CAT_IGNORE && consumed > 0 &&
			    consumed <= len) {
				/* A rig with CI-V transceive enabled echoes our
				 * own command back before answering it. Skip it
				 * and keep reading, rather than reporting
				 * success the radio has not given. */
				memmove(buf, buf + consumed, len - consumed);
				len -= consumed;
				continue;
			}
			if (len >= sizeof buf)
				len = 0;   /* resynchronise */
		}
		if (ardop_mono_ms() > deadline) {
			fprintf(stderr, "ptt: the radio did not answer the %s "
				"keying command within 200 ms\n",
				ardop_cat_family_str(fam));
			p->fault = ARDOP_FAULT_PTT_LOST;
			return;
		}
		ardop_sleep_ms(2);
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
			ardop_cm108_close(p->hid);
	p->hid = NULL;
	ardop_net_close(&p->rig);
			free(p);
			return NULL;
		}
		return p;

	case ARDOP_PTT_CAT:
		if (!serial_open(p)) {
			free(p);
			return NULL;
		}
		if (!serial_configure(p, cfg->baud ? cfg->baud : 19200)) {
			fprintf(stderr, "ptt: cannot configure %s\n", cfg->target);
			serial_close(p);
			free(p);
			return NULL;
		}
		snprintf(p->describe, sizeof(p->describe), "%s:%s@%02x",
			 ardop_cat_family_str((ardop_cat_family)cfg->cat_family),
			 cfg->target, cfg->civ_addr);
		/* Prove the link now rather than at the first transmission, the
		 * same way the rigctld path does. */
		cat_set(p, false);
		if (p->fault != ARDOP_FAULT_NONE) {
			serial_close(p);
			free(p);
			return NULL;
		}
		return p;

	case ARDOP_PTT_CM108: {
		char path[ARDOP_PTT_TARGET_MAX];
		snprintf(path, sizeof path, "%s", cfg->target);

		if (!path[0]) {
			ardop_cm108_candidate cands[16];
			size_t n = ardop_cm108_scan(cands, 16);
			size_t idx = 0;
			char why[512];
			why[0] = '\0';
			if (ardop_cm108_choose(cands, n, cfg->hid_vid,
					       cfg->hid_pid, &idx, why,
					       sizeof why) != 1) {
				fprintf(stderr, "ptt: %s\n", why);
				free(p);
				return NULL;
			}
			snprintf(path, sizeof path, "%s", cands[idx].path);

			/* The pin has to exist on this chip: a default of 3 does
			 * nothing at all on an SSS162x, which has two. */
			unsigned have = ardop_cm108_gpio_count(cands[idx].vid,
							       cands[idx].pid);
			if (have && p->cfg.gpio > have) {
				fprintf(stderr,
					"ptt: %s has %u GPIO pin(s); pin %u does "
					"not exist. Use cm108:...+%u or lower.\n",
					ardop_cm108_chip_name(cands[idx].vid,
							      cands[idx].pid),
					have, p->cfg.gpio, have);
				free(p);
				return NULL;
			}
		}

		p->hid = ardop_cm108_open(path, p->cfg.gpio);
		if (!p->hid) {
			free(p);
			return NULL;
		}
		snprintf(p->describe, sizeof(p->describe), "cm108:%s+%u", path,
			 p->cfg.gpio);
		return p;
	}

	case ARDOP_PTT_GPIO:
		break;
	}

	fprintf(stderr, "ptt: GPIO keying is not implemented yet\n");
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
	case ARDOP_PTT_CAT:
		cat_set(p, key);
		break;
	case ARDOP_PTT_CM108:
		if (!ardop_cm108_set(p->hid, key)) {
			fprintf(stderr, "ptt: CM108 write failed\n");
			p->fault = ARDOP_FAULT_PTT_LOST;
		}
		break;
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
