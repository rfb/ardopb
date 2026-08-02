#include "shell/usbtopo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/radios.h"

#ifndef _WIN32
#  include <dirent.h>
#  include <unistd.h>
#endif

/**
 * @file usbtopo.c
 * @brief Pairing a keying interface with its sound card (see usbtopo.h).
 *
 * The pure half is first and is tested against fabricated device trees, because
 * the machine this was written on has no USB audio at all -- every sound card in
 * it is PCI. The reader is last, is thin, and is the part that has only been run
 * against a machine with nothing to find.
 */

/* --- the relation ---------------------------------------------------------- */

/* Strip the last "port" component: "3-2.4" -> "3-2", "3-2" -> "3". */
static void parent_of(const char *path, char *out, size_t cap)
{
	snprintf(out, cap, "%s", path);
	char *dot = strrchr(out, '.');
	if (dot) {
		*dot = '\0';
		return;
	}
	char *dash = strrchr(out, '-');
	if (dash)
		*dash = '\0';
}

ardop_usb_link ardop_usb_relate(const char *a, const char *b)
{
	if (!a || !b || !*a || !*b)
		return ARDOP_USB_NONE;

	/* Two interfaces of one device share the whole path. Certain. */
	if (strcmp(a, b) == 0)
		return ARDOP_USB_SAME_DEVICE;

	/* Two devices behind one hub share everything but the last port. That is
	 * a DigiRig Mobile, whose C-Media sound card and CP2102 bridge are
	 * separate devices inside one shell -- and it is an inference, not a
	 * certainty, which is why it ranks lower. */
	char pa[256], pb[256];
	parent_of(a, pa, sizeof pa);
	parent_of(b, pb, sizeof pb);
	if (*pa && strcmp(pa, pb) == 0)
		return ARDOP_USB_SAME_HUB;

	return ARDOP_USB_NONE;
}

/* --- pairing --------------------------------------------------------------- */

size_t ardop_usb_pair(const ardop_usb_node *nodes, size_t n,
		      ardop_radio_candidate *out, size_t max)
{
	size_t got = 0;

	for (size_t a = 0; a < n && got < max; a++) {
		if (!nodes[a].is_audio)
			continue;

		/* Best keying interface on the same hardware. */
		size_t best = n;
		ardop_usb_link best_link = ARDOP_USB_NONE;

		for (size_t k = 0; k < n; k++) {
			if (k == a)
				continue;
			if (!nodes[k].is_serial && !nodes[k].is_hid)
				continue;
			if (!nodes[k].devnode[0])
				continue;

			ardop_usb_link rel = ardop_usb_relate(nodes[a].syspath,
							      nodes[k].syspath);
			if (rel == ARDOP_USB_NONE)
				continue;
			/* SAME_DEVICE beats SAME_HUB: the first is certain. */
			if (best_link != ARDOP_USB_NONE && rel >= best_link)
				continue;
			best = k;
			best_link = rel;
		}

		ardop_radio_candidate *c = &out[got++];
		memset(c, 0, sizeof *c);
		snprintf(c->audio_id, sizeof c->audio_id, "%s", nodes[a].devnode);
		c->link = best_link;

		if (best == n)
			continue;   /* a sound card with no keying sibling */

		c->vid = nodes[best].vid;
		c->pid = nodes[best].pid;

		const ardop_radio_entry *e = ardop_radio_lookup(c->vid, c->pid);
		if (e) {
			snprintf(c->model, sizeof c->model, "%s", e->model);
			c->ptt_is_guess = ardop_radio_ptt_spec(
				e, nodes[best].devnode, c->ptt_spec,
				sizeof c->ptt_spec);
		}

		if (!c->ptt_spec[0]) {
			/* Nothing in the table. A serial port is most likely RTS
			 * and a HID device is most likely CM108 -- offered as a
			 * starting point, and clearly not a guess the table
			 * stands behind. */
			snprintf(c->ptt_spec, sizeof c->ptt_spec, "%s%s",
				 nodes[best].is_hid ? "cm108:" : "rts:",
				 nodes[best].devnode);
			c->ptt_is_guess = false;
		}
	}
	return got;
}

/* --- reading the real tree ------------------------------------------------- */

#ifndef _WIN32

/* Read one line of a sysfs attribute. */
static bool slurp(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (!ok)
		return false;
	size_t n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return true;
}

/*
 * Extract the USB device path from a sysfs device link.
 *
 * A sound card resolves to something like
 *   /sys/devices/pci0000:00/.../usb3/3-2/3-2.4/3-2.4:1.0/sound/card4
 * and the USB *device* is "3-2.4" -- the last path component before the
 * interface, which carries a colon. That component is the pairing key.
 */
static bool usb_device_of(const char *link, char *out, size_t cap)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s", link);

	/* Walk backwards for the last component that looks like a USB device:
	 * digits, a dash, then digits and dots, with no colon. */
	char *p = buf + strlen(buf);
	while (p > buf) {
		char *slash = strrchr(buf, '/');
		if (!slash)
			break;
		*slash = '\0';
		p = slash + 1;

		if (!strchr(p, ':') && strchr(p, '-')) {
			bool plausible = true;
			for (const char *q = p; *q; q++)
				if (!((*q >= '0' && *q <= '9') || *q == '-' ||
				      *q == '.'))
					plausible = false;
			if (plausible) {
				snprintf(out, cap, "%s", p);
				return true;
			}
		}
	}
	return false;
}

static void read_ids(const char *root, const char *usbdev, uint16_t *vid,
		     uint16_t *pid)
{
	char path[512], buf[64];
	*vid = *pid = 0;

	snprintf(path, sizeof path, "%s/sys/bus/usb/devices/%s/idVendor", root,
		 usbdev);
	if (slurp(path, buf, sizeof buf))
		*vid = (uint16_t)strtoul(buf, NULL, 16);

	snprintf(path, sizeof path, "%s/sys/bus/usb/devices/%s/idProduct", root,
		 usbdev);
	if (slurp(path, buf, sizeof buf))
		*pid = (uint16_t)strtoul(buf, NULL, 16);
}

/* Add every entry of one /sys/class directory that resolves to a USB device. */
static size_t scan_class(const char *root, const char *cls, const char *prefix,
			 bool is_audio, bool is_serial, bool is_hid,
			 ardop_usb_node *out, size_t max, size_t got)
{
	char dir[384];
	snprintf(dir, sizeof dir, "%s/sys/class/%s", root, cls);

	DIR *d = opendir(dir);
	if (!d)
		return got;

	struct dirent *e;
	while (got < max && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0)
			continue;

		/* The fixture trees the tests build are plain directories with a
		 * "usbpath" file naming the device, because a test cannot create
		 * the symlink farm the kernel does. The live reader prefers the
		 * real link and falls back to that. */
		char link[1024], usbdev[128];
		snprintf(link, sizeof link, "%s/%.255s/device", dir, e->d_name);

		char resolved[512];
		bool have = false;
		char hint[1024];
		snprintf(hint, sizeof hint, "%s/%.255s/usbpath", dir, e->d_name);
		if (slurp(hint, usbdev, sizeof usbdev)) {
			have = true;
		} else {
			ssize_t n = readlink(link, resolved, sizeof resolved - 1);
			if (n > 0) {
				resolved[n] = '\0';
				have = usb_device_of(resolved, usbdev,
						     sizeof usbdev);
			}
		}
		if (!have)
			continue;

		ardop_usb_node *nd = &out[got++];
		memset(nd, 0, sizeof *nd);
		snprintf(nd->syspath, sizeof nd->syspath, "%s", usbdev);
		snprintf(nd->devnode, sizeof nd->devnode, "/dev/%.*s",
			 (int)(sizeof nd->devnode - 6), e->d_name);
		nd->is_audio = is_audio;
		nd->is_serial = is_serial;
		nd->is_hid = is_hid;
		read_ids(root, usbdev, &nd->vid, &nd->pid);
	}
	closedir(d);
	return got;
}

size_t ardop_usb_scan(const char *root, ardop_usb_node *out, size_t max)
{
	if (!root)
		root = "";

	size_t got = 0;
	got = scan_class(root, "sound", "card", true, false, false, out, max, got);
	got = scan_class(root, "tty", "ttyUSB", false, true, false, out, max, got);
	got = scan_class(root, "tty", "ttyACM", false, true, false, out, max, got);
	got = scan_class(root, "hidraw", "hidraw", false, false, true, out, max,
			 got);
	return got;
}

#else /* _WIN32 */

/*
 * Windows groups every function of one physical device under a shared Container
 * ID, which is the same idea as the ancestor walk with none of the walking.
 * Reading it needs the CM_Get_DevNode_Property family; that is not implemented
 * here yet, so detection reports nothing on Windows and the manual pickers are
 * the whole interface, exactly as before.
 *
 * The pure half above is built and tested on both platforms, so this is a
 * reader to write, not a design to redo.
 */
size_t ardop_usb_scan(const char *root, ardop_usb_node *out, size_t max)
{
	(void)root;
	(void)out;
	(void)max;
	return 0;
}

#endif

size_t ardop_radio_detect(ardop_radio_candidate *out, size_t max,
			  const char *backend_name)
{
	static ardop_usb_node nodes[64];
	static ardop_audio_device devs[64];

	size_t n = ardop_usb_scan(NULL, nodes, sizeof nodes / sizeof nodes[0]);
	size_t got = ardop_usb_pair(nodes, n, out, max);

	/*
	 * The walk knows sound cards by their kernel name ("/dev/card1"), and the
	 * application selects them by the id miniaudio renders. Match the two up
	 * by card index where we can, and leave the name blank rather than
	 * guessing when we cannot -- a candidate whose audio device cannot be
	 * identified is worse than no candidate.
	 */
	size_t ndev = ardop_audio_enumerate(ARDOP_AUDIO_CAPTURE, devs,
					    sizeof devs / sizeof devs[0],
					    backend_name);
	for (size_t i = 0; i < got; i++) {
		const char *card = strstr(out[i].audio_id, "card");
		if (!card)
			continue;
		for (size_t j = 0; j < ndev; j++) {
			if (!strstr(devs[j].id, card + 4))
				continue;
			snprintf(out[i].audio_id, sizeof out[i].audio_id, "%s",
				 devs[j].id);
			snprintf(out[i].audio_name, sizeof out[i].audio_name,
				 "%s", devs[j].name);
			break;
		}
	}
	return got;
}
