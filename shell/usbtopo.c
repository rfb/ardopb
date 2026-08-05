#include "shell/usbtopo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/radios.h"

#ifndef _WIN32
#  include <dirent.h>
#  include <limits.h>
#  include <stdlib.h>
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

/* --- naming the same sound card twice --------------------------------------- */

/*
 * The walk knows a sound card as the kernel does, by index; the application
 * selects one by whatever string the audio backend renders. Neither name can be
 * turned into the other, so this matches them by the two things they can be
 * made to share -- the index, where the backend uses one, and udev's name for
 * the USB device, which every Pulse-style id embeds.
 */
bool ardop_usb_audio_id_matches(const ardop_usb_node *nd, const char *id)
{
	if (!nd || !id || !*id)
		return false;

	const char *card = strstr(nd->devnode, "card");

	/* ALSA names a card by index and nothing else: "hw:1,0". */
	if (card && strncmp(id, "hw:", 3) == 0)
		return strtol(card + 4, NULL, 10) == strtol(id + 3, NULL, 10);

	return nd->usbkey[0] && strstr(id, nd->usbkey) != NULL;
}

/* --- pairing --------------------------------------------------------------- */

/* One audio node paired with one keying node, as the application sees it. */
static void describe_pair(ardop_radio_candidate *c, const ardop_usb_node *audio,
			  const ardop_usb_node *key, ardop_usb_link link)
{
	memset(c, 0, sizeof *c);
	snprintf(c->audio_id, sizeof c->audio_id, "%s", audio->devnode);
	c->link = link;

	if (!key)
		return;   /* a sound card with no keying sibling */

	c->vid = key->vid;
	c->pid = key->pid;

	const ardop_radio_entry *e = ardop_radio_lookup(c->vid, c->pid);
	if (e) {
		snprintf(c->model, sizeof c->model, "%s", e->model);
		c->ptt_is_guess = ardop_radio_ptt_spec(e, key->devnode,
						       c->ptt_spec,
						       sizeof c->ptt_spec);
	}

	if (!c->ptt_spec[0]) {
		/* Nothing in the table. A serial port is most likely RTS and a
		 * HID device is most likely CM108 -- offered as a starting
		 * point, and clearly not a guess the table stands behind. */
		snprintf(c->ptt_spec, sizeof c->ptt_spec, "%s%s",
			 key->is_hid ? "cm108:" : "rts:", key->devnode);
		c->ptt_is_guess = false;
	}
}

size_t ardop_usb_pair(const ardop_usb_node *nodes, size_t n,
		      ardop_radio_candidate *out, size_t max)
{
	/* Certain pairings before inferred ones. Both are offered: see below. */
	static const ardop_usb_link kRanks[] = {ARDOP_USB_SAME_DEVICE,
						ARDOP_USB_SAME_HUB};
	size_t got = 0;

	for (size_t a = 0; a < n && got < max; a++) {
		if (!nodes[a].is_audio)
			continue;

		size_t before = got;

		/*
		 * *Every* keying interface on the same hardware, not only the
		 * best-ranked one.
		 *
		 * Offering just the winner hid a DigiRig Mobile's serial bridge
		 * behind its own codec's HID interface, whose GPIO pin is not
		 * bonded to anything -- so the only method offered was the one
		 * that cannot key, and it failed silently
		 * ([20](../analysis/20-field-results.md) finding 3).
		 *
		 * The ranking answers "which device is this", which it does
		 * well. It was being read as "which line keys", and that
		 * question is not answerable from the USB tree at all: the same
		 * chip appears whether or not the pin goes anywhere. Only the
		 * operator's radio can settle it, so list what exists, in
		 * confidence order, and let the PTT test choose.
		 */
		for (size_t r = 0; r < sizeof kRanks / sizeof kRanks[0]
				   && got < max; r++) {
			for (size_t k = 0; k < n && got < max; k++) {
				if (k == a)
					continue;
				if (!nodes[k].is_serial && !nodes[k].is_hid)
					continue;
				if (!nodes[k].devnode[0])
					continue;
				if (ardop_usb_relate(nodes[a].syspath,
						     nodes[k].syspath) != kRanks[r])
					continue;

				describe_pair(&out[got++], &nodes[a], &nodes[k],
					      kRanks[r]);
			}
		}

		/* A sound card with nothing on its hardware is still named, so
		 * that an operator can see it was considered and reach for the
		 * manual pickers rather than wonder. */
		if (got == before)
			describe_pair(&out[got++], &nodes[a], NULL,
				      ARDOP_USB_NONE);
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

/*
 * The name udev builds for a USB device: manufacturer, product and serial,
 * joined by underscores, with every space turned into one too --
 * "C-Media_Electronics_Inc._USB_Audio_Device". It is worth reading because
 * every id a PulseAudio-style backend renders contains it, and it is the only
 * thing the kernel's view of a sound card and the backend's view have in
 * common: a card *index* does not appear in a Pulse id at all.
 */
static void read_key(const char *root, const char *usbdev, char *out, size_t cap)
{
	static const char *const kAttrs[] = {"manufacturer", "product", "serial"};
	size_t used = 0;

	if (cap)
		out[0] = '\0';

	for (size_t i = 0; i < sizeof kAttrs / sizeof kAttrs[0]; i++) {
		char path[512], val[192];
		snprintf(path, sizeof path, "%s/sys/bus/usb/devices/%s/%s", root,
			 usbdev, kAttrs[i]);
		if (!slurp(path, val, sizeof val) || !val[0])
			continue;

		for (char *p = val; *p; p++)
			if (*p == ' ')
				*p = '_';

		int n = snprintf(out + used, cap - used, "%s%s",
				 used ? "_" : "", val);
		if (n < 0 || (size_t)n >= cap - used) {
			out[used] = '\0';   /* keep what fits, whole */
			return;
		}
		used += (size_t)n;
	}
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

		bool have = false;
		char hint[1024];
		snprintf(hint, sizeof hint, "%s/%.255s/usbpath", dir, e->d_name);
		if (slurp(hint, usbdev, sizeof usbdev)) {
			have = true;
		} else {
			/* Resolved, not read. The kernel stores these links
			 * relative and short -- /sys/class/sound/card0/device is
			 * "../../../1-2.2:1.0" -- so reading the link gives an
			 * interface name with no device path above it, and the
			 * walk below has nothing to find. realpath() puts the
			 * ancestry back. */
			char *abs = realpath(link, NULL);
			if (abs) {
				have = usb_device_of(abs, usbdev, sizeof usbdev);
				free(abs);
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
		read_key(root, usbdev, nd->usbkey, sizeof nd->usbkey);
	}
	closedir(d);
	return got;
}

bool ardop_usb_detect_supported(void)
{
	return true;
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
bool ardop_usb_detect_supported(void)
{
	return false;
}

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
	 * application selects them by the id the audio backend renders. Match the
	 * two up, and blank the id rather than guessing when they cannot be
	 * matched -- a candidate carrying a name the pickers do not recognise
	 * points an operator at the wrong sound card, which is worse than a
	 * candidate that admits it does not know.
	 */
	size_t ndev = ardop_audio_enumerate(ARDOP_AUDIO_CAPTURE, devs,
					    sizeof devs / sizeof devs[0],
					    backend_name);
	for (size_t i = 0; i < got; i++) {
		const ardop_usb_node *nd = NULL;
		for (size_t k = 0; k < n; k++)
			if (nodes[k].is_audio &&
			    strcmp(nodes[k].devnode, out[i].audio_id) == 0) {
				nd = &nodes[k];
				break;
			}

		out[i].audio_id[0] = '\0';
		out[i].audio_name[0] = '\0';
		if (!nd)
			continue;

		for (size_t j = 0; j < ndev; j++) {
			if (!ardop_usb_audio_id_matches(nd, devs[j].id))
				continue;
			/* A monitor carries the same identity as the card it
			 * echoes, and is the playback stream fed back rather
			 * than anything the radio sent. Never the capture
			 * device an operator means. */
			size_t len = strlen(devs[j].id);
			if (len >= 8 && strcmp(devs[j].id + len - 8, ".monitor") == 0)
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
