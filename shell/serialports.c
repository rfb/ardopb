#include "shell/serialports.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <setupapi.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

/**
 * @file serialports.c
 * @brief Listing serial ports (see serialports.h).
 */

#ifndef _WIN32

/* Read a one-line sysfs attribute, trimmed. */
static bool attr(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (!ok)
		return false;
	size_t n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
		out[--n] = '\0';
	return out[0] != '\0';
}

/*
 * A friendly name for a tty, from the USB device it belongs to.
 *
 * sysfs puts `product` and `manufacturer` on the USB *device*, several levels
 * above the tty, so this walks up until it finds one or runs out. A port with no
 * USB parent -- a real 16550 on a motherboard -- simply has no name, and the
 * caller falls back to the node.
 */
static bool usb_name(const char *tty, char *out, size_t cap)
{
	char dir[512];
	snprintf(dir, sizeof dir, "/sys/class/tty/%s/device", tty);

	for (int up = 0; up < 6; up++) {
		char product[192], vendor[192], path[640];

		snprintf(path, sizeof path, "%s/product", dir);
		if (attr(path, product, sizeof product)) {
			snprintf(path, sizeof path, "%s/manufacturer", dir);
			if (attr(path, vendor, sizeof vendor) &&
			    strstr(product, vendor) == NULL)
				snprintf(out, cap, "%s %s", vendor, product);
			else
				snprintf(out, cap, "%s", product);
			return true;
		}

		size_t n = strlen(dir);
		if (n + 4 >= sizeof dir)
			return false;
		memcpy(dir + n, "/..", 4);
	}
	return false;
}

size_t ardop_serial_ports(ardop_serial_port *out, size_t max)
{
	DIR *d = opendir("/sys/class/tty");
	if (!d)
		return 0;

	size_t n = 0;
	struct dirent *e;
	while (n < max && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;

		/*
		 * A driver bound to it is what makes a port real. Every machine
		 * has ttyS0 through ttyS31 in /dev whether or not anything is
		 * behind them, and a list that long is one nobody reads.
		 */
		char probe[512];
		snprintf(probe, sizeof probe, "/sys/class/tty/%.128s/device/driver",
			 e->d_name);
		if (access(probe, F_OK) != 0)
			continue;

		ardop_serial_port *p = &out[n];
		memset(p, 0, sizeof *p);
		snprintf(p->path, sizeof p->path, "/dev/%.120s", e->d_name);

		/* Bounded explicitly: the compiler cannot see that a sysfs
		 * product string and a device node together fit, and a name that
		 * silently lost its path would be a name an operator could not
		 * act on. */
		char friendly[192];
		if (usb_name(e->d_name, friendly, sizeof friendly))
			snprintf(p->name, sizeof p->name, "%.150s (%.28s)",
				 friendly, p->path);
		else
			snprintf(p->name, sizeof p->name, "%s", p->path);
		n++;
	}
	closedir(d);
	return n;
}

#else /* _WIN32 */

size_t ardop_serial_ports(ardop_serial_port *out, size_t max)
{
	/*
	 * The Ports class, with each device's friendly name -- which Windows
	 * already formats as "Silicon Labs CP210x USB to UART Bridge (COM3)".
	 * That string is exactly what an operator is looking for, so it is used
	 * as-is and the COM name is pulled back out of it for the path.
	 */
	static const GUID kPortsClass = {
		0x4d36e978, 0xe325, 0x11ce,
		{0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}
	};

	HDEVINFO set = SetupDiGetClassDevsA(&kPortsClass, NULL, NULL, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE)
		return 0;

	size_t n = 0;
	SP_DEVINFO_DATA info;
	info.cbSize = sizeof info;

	for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(set, i, &info); i++) {
		char friendly[ARDOP_PORT_NAME_MAX] = {0};
		if (!SetupDiGetDeviceRegistryPropertyA(
			    set, &info, SPDRP_FRIENDLYNAME, NULL,
			    (PBYTE)friendly, sizeof friendly - 1, NULL))
			continue;

		/* "... (COM12)" -- take what is inside the last parentheses. */
		char com[32] = {0};
		const char *open_paren = strrchr(friendly, '(');
		if (open_paren) {
			const char *close_paren = strchr(open_paren, ')');
			if (close_paren && close_paren - open_paren - 1 <
						  (long)sizeof com) {
				size_t len = (size_t)(close_paren - open_paren - 1);
				memcpy(com, open_paren + 1, len);
				com[len] = '\0';
			}
		}
		if (strncmp(com, "COM", 3) != 0)
			continue;   /* an LPT port, or something without one */

		ardop_serial_port *p = &out[n];
		memset(p, 0, sizeof *p);
		/*
		 * The bare name, not the \\.\ form: ardop_ptt_parse applies that
		 * prefix itself for COM10 and above, and a spec an operator can
		 * read back is worth more than one that is already escaped.
		 */
		snprintf(p->path, sizeof p->path, "%s", com);
		snprintf(p->name, sizeof p->name, "%s", friendly);
		n++;
	}

	SetupDiDestroyDeviceInfoList(set);
	return n;
}

#endif /* _WIN32 */
