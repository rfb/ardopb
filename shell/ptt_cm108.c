#include "shell/ptt_cm108.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#endif

/**
 * @file ptt_cm108.c
 * @brief C-Media GPIO keying (see ptt_cm108.h).
 *
 * The pure half is first and is exhaustively tested. The device half is last,
 * is as thin as it can be made, and **has never been run against hardware**.
 */

/* --- the chip table -------------------------------------------------------- */

/*
 * From direwolf's cm108.c, which is field-verified against interfaces this
 * project does not own. The GPIO counts are the load-bearing column: defaulting
 * to pin 3 on an SSS162x, which has two, sets a bit that does not exist.
 *
 * Ranges rather than single ids because CM108/CM109/CM119 share 0x0008-0x000f
 * depending on how the interface was configured at manufacture.
 */
static const struct {
	uint16_t vid, pid_lo, pid_hi;
	unsigned gpios;
	const char *name;
} kChips[] = {
	{0x0d8c, 0x0008, 0x000f, 8, "C-Media CM108/CM109/CM119"},
	{0x0d8c, 0x0012, 0x0012, 3, "C-Media CM108B"},
	{0x0d8c, 0x0013, 0x0013, 8, "C-Media CM119B"},
	{0x0d8c, 0x0139, 0x0139, 3, "C-Media CM108AH"},
	{0x0d8c, 0x013a, 0x013a, 8, "C-Media CM119A"},
	{0x0d8c, 0x013c, 0x013c, 3, "C-Media CM108AH"},
	{0x0c76, 0x1605, 0x1605, 2, "SSS1621"},
	{0x0c76, 0x1607, 0x1607, 2, "SSS1623"},
	{0x0c76, 0x160b, 0x160b, 2, "SSS1623"},
};

#define NCHIPS (sizeof kChips / sizeof kChips[0])

static const char *lookup(uint16_t vid, uint16_t pid, unsigned *gpios)
{
	for (size_t i = 0; i < NCHIPS; i++) {
		if (vid != kChips[i].vid)
			continue;
		if (pid < kChips[i].pid_lo || pid > kChips[i].pid_hi)
			continue;
		if (gpios)
			*gpios = kChips[i].gpios;
		return kChips[i].name;
	}
	if (gpios)
		*gpios = 0;
	return NULL;
}

unsigned ardop_cm108_gpio_count(uint16_t vid, uint16_t pid)
{
	unsigned n = 0;
	(void)lookup(vid, pid, &n);
	return n;
}

const char *ardop_cm108_chip_name(uint16_t vid, uint16_t pid)
{
	return lookup(vid, pid, NULL);
}

bool ardop_cm108_is_known(uint16_t vid, uint16_t pid)
{
	return lookup(vid, pid, NULL) != NULL;
}

/* --- the report ------------------------------------------------------------ */

bool ardop_cm108_report(unsigned gpio, bool key,
			uint8_t out[ARDOP_CM108_REPORT_LEN])
{
	if (gpio < 1 || gpio > 8)
		return false;

	uint8_t mask = (uint8_t)(1u << (gpio - 1));

	out[0] = 0x00;               /* report id */
	out[1] = 0x00;               /* reserved */
	out[2] = key ? mask : 0x00;  /* GPIO levels */
	out[3] = mask;               /* direction: this pin is an output */
	out[4] = 0x00;               /* reserved -- see the header. */
	return true;
}

/* --- the auto policy ------------------------------------------------------- */

int ardop_cm108_choose(const ardop_cm108_candidate *cands, size_t n,
		       uint16_t want_vid, uint16_t want_pid, size_t *index,
		       char *why, size_t why_cap)
{
	size_t hits = 0, first = 0;

	for (size_t i = 0; i < n; i++) {
		bool ok = want_vid ? (cands[i].vid == want_vid &&
				      cands[i].pid == want_pid)
				   : ardop_cm108_is_known(cands[i].vid,
							  cands[i].pid);
		if (!ok)
			continue;
		if (hits == 0)
			first = i;
		hits++;
	}

	if (hits == 1) {
		*index = first;
		return 1;
	}

	if (hits == 0) {
		snprintf(why, why_cap,
			 "no C-Media HID device found. Plug the interface in, or "
			 "name it explicitly as cm108:/dev/hidrawN.");
		return 0;
	}

	/* Two identical dongles is exactly the case where guessing keys the wrong
	 * radio, so name them all and let the operator choose. */
	int off = snprintf(why, why_cap,
			   "%zu C-Media HID devices found; name one explicitly:",
			   hits);
	for (size_t i = 0; i < n && off > 0 && (size_t)off < why_cap; i++) {
		bool ok = want_vid ? (cands[i].vid == want_vid &&
				      cands[i].pid == want_pid)
				   : ardop_cm108_is_known(cands[i].vid,
							  cands[i].pid);
		if (!ok)
			continue;
		off += snprintf(why + off, why_cap - (size_t)off,
				" cm108:%s (%04x:%04x)", cands[i].path,
				cands[i].vid, cands[i].pid);
	}
	return -1;
}

/* --- the sysfs parser ------------------------------------------------------ */

bool ardop_cm108_parse_hid_id(const char *line, uint16_t *vid, uint16_t *pid)
{
	if (!line)
		return false;

	const char *p = strstr(line, "HID_ID=");
	if (!p)
		return false;
	p += 7;

	unsigned bus = 0, v = 0, d = 0;
	if (sscanf(p, "%x:%x:%x", &bus, &v, &d) != 3)
		return false;

	/* 0003 is USB. 0005 is Bluetooth, which has no GPIO to key with. */
	if (bus != 0x0003)
		return false;
	if (v > 0xFFFFu || d > 0xFFFFu)
		return false;

	*vid = (uint16_t)v;
	*pid = (uint16_t)d;
	return true;
}

/* --- the device half ------------------------------------------------------- */
/*
 * Everything below touches an actual device and has never been run against one.
 * It is kept deliberately free of logic so that the untested surface is as small
 * as it can be.
 */

struct ardop_cm108 {
	unsigned gpio;
#ifdef _WIN32
	HANDLE h;
#else
	int fd;
#endif
};

#ifndef _WIN32

/* Read one line out of a sysfs file. */
static bool read_first_match(const char *path, const char *want, char *out,
			     size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool got = false;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (strstr(line, want)) {
			snprintf(out, cap, "%s", line);
			got = true;
			break;
		}
	}
	fclose(f);
	return got;
}

size_t ardop_cm108_scan(ardop_cm108_candidate *out, size_t max)
{
	/*
	 * sysfs, not the devices themselves. Opening every hidraw node to
	 * identify it would fail on exactly the devices whose permissions are
	 * wrong -- which is the common case and the one where a useful message
	 * matters most.
	 */
	DIR *d = opendir("/sys/class/hidraw");
	if (!d)
		return 0;

	size_t n = 0;
	struct dirent *e;
	while (n < max && (e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "hidraw", 6) != 0)
			continue;

		char uevent[320];
		snprintf(uevent, sizeof uevent,
			 "/sys/class/hidraw/%s/device/uevent", e->d_name);

		char line[256];
		if (!read_first_match(uevent, "HID_ID=", line, sizeof line))
			continue;

		uint16_t vid = 0, pid = 0;
		if (!ardop_cm108_parse_hid_id(line, &vid, &pid))
			continue;

		/* A precision, not just a size: a directory entry may be up to
		 * NAME_MAX, and the compiler cannot see that hidraw names are
		 * short. A path we could not represent whole is one we could
		 * never open, so bounding it here is honest as well as quiet. */
		snprintf(out[n].path, sizeof out[n].path, "/dev/%.*s",
			 (int)(sizeof out[n].path - 6), e->d_name);
		out[n].vid = vid;
		out[n].pid = pid;
		n++;
	}
	closedir(d);
	return n;
}

ardop_cm108 *ardop_cm108_open(const char *path, unsigned gpio)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM) {
			/* A message that says "permission denied" and stops is a
			 * support ticket. Print the fix. */
			fprintf(stderr,
"ptt: found a C-Media HID at %s but cannot open it (permission denied).\n"
"     Create /etc/udev/rules.d/99-ardop-cm108.rules containing:\n"
"\n"
"       SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"0d8c\", MODE=\"0660\", \\\n"
"           GROUP=\"plugdev\", TAG+=\"uaccess\"\n"
"\n"
"     then: sudo udevadm control --reload && sudo udevadm trigger\n"
"     and replug the interface. TAG+=\"uaccess\" is enough on systemd systems\n"
"     without joining plugdev; the GROUP is the fallback for others.\n",
				path);
		} else {
			fprintf(stderr, "ptt: cannot open %s\n", path);
		}
		return NULL;
	}

	ardop_cm108 *c = calloc(1, sizeof *c);
	if (!c) {
		close(fd);
		return NULL;
	}
	c->fd = fd;
	c->gpio = gpio;
	return c;
}

bool ardop_cm108_set(ardop_cm108 *c, bool key)
{
	uint8_t rep[ARDOP_CM108_REPORT_LEN];
	if (!c || !ardop_cm108_report(c->gpio, key, rep))
		return false;
	return write(c->fd, rep, sizeof rep) == (ssize_t)sizeof rep;
}

void ardop_cm108_close(ardop_cm108 *c)
{
	if (!c)
		return;
	(void)ardop_cm108_set(c, false);
	close(c->fd);
	free(c);
}

#else /* _WIN32 */

/*
 * hid.dll and setupapi.dll both ship with Windows, so these are link additions
 * rather than anything an operator installs. They are resolved at load rather
 * than at run time because both are always present.
 */
#include <setupapi.h>

typedef void(__stdcall *hid_get_guid_fn)(GUID *);
typedef BOOLEAN(__stdcall *hid_set_report_fn)(HANDLE, PVOID, ULONG);

static hid_get_guid_fn hid_get_guid;
static hid_set_report_fn hid_set_report;

static bool hid_load(void)
{
	if (hid_set_report)
		return true;
	HMODULE m = LoadLibraryA("hid.dll");
	if (!m)
		return false;
	hid_get_guid = (hid_get_guid_fn)(void *)GetProcAddress(m, "HidD_GetHidGuid");
	hid_set_report = (hid_set_report_fn)(void *)GetProcAddress(
		m, "HidD_SetOutputReport");
	return hid_get_guid && hid_set_report;
}

size_t ardop_cm108_scan(ardop_cm108_candidate *out, size_t max)
{
	if (!hid_load())
		return 0;

	GUID guid;
	hid_get_guid(&guid);

	HDEVINFO set = SetupDiGetClassDevsA(&guid, NULL, NULL,
					    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE)
		return 0;

	size_t n = 0;
	SP_DEVICE_INTERFACE_DATA ifd;
	ifd.cbSize = sizeof ifd;
	for (DWORD i = 0; n < max &&
	     SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &ifd); i++) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
		if (need == 0 || need > 2048)
			continue;

		SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = calloc(1, need);
		if (!det)
			continue;
		det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
		if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL,
						     NULL)) {
			/* The interface path carries the ids in text, so no
			 * device has to be opened to identify it -- the same
			 * property the Linux sysfs walk relies on. */
			unsigned vid = 0, pid = 0;
			const char *v = strstr(det->DevicePath, "vid_");
			const char *p = strstr(det->DevicePath, "pid_");
			if (v && p && sscanf(v + 4, "%4x", &vid) == 1 &&
			    sscanf(p + 4, "%4x", &pid) == 1) {
				snprintf(out[n].path, sizeof out[n].path, "%s",
					 det->DevicePath);
				out[n].vid = (uint16_t)vid;
				out[n].pid = (uint16_t)pid;
				n++;
			}
		}
		free(det);
	}
	SetupDiDestroyDeviceInfoList(set);
	return n;
}

ardop_cm108 *ardop_cm108_open(const char *path, unsigned gpio)
{
	if (!hid_load()) {
		fprintf(stderr, "ptt: cannot load hid.dll\n");
		return NULL;
	}

	HANDLE h = CreateFileA(path, GENERIC_WRITE,
			       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			       OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fprintf(stderr,
			"ptt: cannot open %s. Another program may be holding "
			"the device open.\n", path);
		return NULL;
	}

	ardop_cm108 *c = calloc(1, sizeof *c);
	if (!c) {
		CloseHandle(h);
		return NULL;
	}
	c->h = h;
	c->gpio = gpio;
	return c;
}

bool ardop_cm108_set(ardop_cm108 *c, bool key)
{
	uint8_t rep[ARDOP_CM108_REPORT_LEN];
	if (!c || !ardop_cm108_report(c->gpio, key, rep))
		return false;
	/* HidD_SetOutputReport (a control transfer) rather than WriteFile (an
	 * interrupt transfer): it tolerates a length mismatch, and a CM108's
	 * PTT report is a control report in every implementation of this. */
	return hid_set_report(c->h, rep, sizeof rep) != 0;
}

void ardop_cm108_close(ardop_cm108 *c)
{
	if (!c)
		return;
	(void)ardop_cm108_set(c, false);
	CloseHandle(c->h);
	free(c);
}

#endif /* _WIN32 */
