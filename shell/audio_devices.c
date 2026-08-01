#include "shell/audio_devices.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "shell/ma_common.h"

/**
 * @file audio_devices.c
 * @brief Device enumeration over miniaudio (see audio_devices.h).
 */

/*
 * A device ID is a union whose active member depends on the backend, so it has
 * to be rendered per backend rather than blindly hexed. Producing something an
 * operator can read and retype matters: these strings end up in a config file
 * and on a command line.
 *
 * There is deliberately no inverse. Resolving a saved ID means enumerating and
 * comparing the rendered strings, which cannot disagree with what was shown to
 * the operator -- a hand-written parser could.
 */
void ardop_ma_id_to_str(const ma_device_id *id, ma_backend backend, char *out,
			size_t cap)
{
	if (!id || cap == 0) {
		if (cap)
			out[0] = '\0';
		return;
	}

	switch (backend) {
	case ma_backend_wasapi: {
		/* UTF-16 endpoint path. Transcode the ASCII range directly --
		 * Windows endpoint IDs are `{GUID}.{GUID}` and contain nothing
		 * else -- and refuse to guess at anything wider. */
		size_t w = 0;
		for (size_t i = 0; i < 64 && id->wasapi[i] && w + 1 < cap; i++) {
			unsigned c = (unsigned)id->wasapi[i];
			out[w++] = (c < 0x80u) ? (char)c : '?';
		}
		out[w] = '\0';
		return;
	}
	case ma_backend_dsound: {
		static const char hex[] = "0123456789abcdef";
		size_t w = 0;
		for (size_t i = 0; i < 16 && w + 2 < cap; i++) {
			out[w++] = hex[(id->dsound[i] >> 4) & 0xF];
			out[w++] = hex[id->dsound[i] & 0xF];
		}
		out[w] = '\0';
		return;
	}
	case ma_backend_winmm:
		snprintf(out, cap, "%u", (unsigned)id->winmm);
		return;
	case ma_backend_alsa:
		snprintf(out, cap, "%.*s", (int)sizeof(id->alsa), id->alsa);
		return;
	case ma_backend_pulseaudio:
		snprintf(out, cap, "%.*s", (int)sizeof(id->pulse), id->pulse);
		return;
	case ma_backend_coreaudio:
		snprintf(out, cap, "%.*s", (int)sizeof(id->coreaudio),
			 id->coreaudio);
		return;
	case ma_backend_sndio:
		snprintf(out, cap, "%.*s", (int)sizeof(id->sndio), id->sndio);
		return;
	case ma_backend_audio4:
		snprintf(out, cap, "%.*s", (int)sizeof(id->audio4), id->audio4);
		return;
	case ma_backend_oss:
		snprintf(out, cap, "%.*s", (int)sizeof(id->oss), id->oss);
		return;
	case ma_backend_jack:
		snprintf(out, cap, "%d", id->jack);
		return;
	case ma_backend_aaudio:
		snprintf(out, cap, "%d", (int)id->aaudio);
		return;
	case ma_backend_opensl:
		snprintf(out, cap, "%u", (unsigned)id->opensl);
		return;
	case ma_backend_webaudio:
		snprintf(out, cap, "%.*s", (int)sizeof(id->webaudio),
			 id->webaudio);
		return;
	case ma_backend_null:
		snprintf(out, cap, "null%d", id->nullbackend);
		return;
	case ma_backend_custom:
		snprintf(out, cap, "%.*s", (int)sizeof(id->custom.s),
			 id->custom.s);
		return;
	}
	snprintf(out, cap, "?");
}

/* Strip case, spaces, dashes and underscores so "core audio", "CoreAudio" and
 * "core-audio" all match the display name "Core Audio". */
static void normalise(const char *in, char *out, size_t cap)
{
	size_t w = 0;
	for (; *in && w + 1 < cap; in++) {
		if (*in == ' ' || *in == '-' || *in == '_')
			continue;
		out[w++] = (char)tolower((unsigned char)*in);
	}
	out[w] = '\0';
}

bool ardop_ma_backend_from_name(const char *name, ma_backend *out)
{
	char want[64], have[64];
	normalise(name, want, sizeof(want));

	for (int i = 0; i <= (int)ma_backend_null; i++) {
		ma_backend b = (ma_backend)i;
		const char *n = ma_get_backend_name(b);
		if (!n)
			continue;
		normalise(n, have, sizeof(have));
		if (strcmp(want, have) == 0) {
			if (!ma_is_backend_enabled(b)) {
				fprintf(stderr, "audio: backend '%s' is not "
					"available on this platform\n", name);
				return false;
			}
			*out = b;
			return true;
		}
	}

	fprintf(stderr, "audio: unknown backend '%s'. Available here:", name);
	for (int i = 0; i <= (int)ma_backend_null; i++) {
		ma_backend b = (ma_backend)i;
		if (ma_is_backend_enabled(b))
			fprintf(stderr, " %s", ma_get_backend_name(b));
	}
	fprintf(stderr, "\n");
	return false;
}

/* Bring up a context, optionally pinned to one named backend. */
static bool context_init(ma_context *ctx, const char *backend_name)
{
	if (backend_name && *backend_name) {
		ma_backend only[1];
		if (!ardop_ma_backend_from_name(backend_name, &only[0]))
			return false;
		return ma_context_init(only, 1, NULL, ctx) == MA_SUCCESS;
	}
	return ma_context_init(NULL, 0, NULL, ctx) == MA_SUCCESS;
}

size_t ardop_audio_enumerate(ardop_audio_dir dir, ardop_audio_device *out,
			     size_t max, const char *backend_name)
{
	if (max == 0)
		return 0;

	ma_context ctx;
	if (!context_init(&ctx, backend_name)) {
		fprintf(stderr, "audio: cannot initialise an audio context\n");
		return 0;
	}

	ma_device_info *play = NULL, *cap = NULL;
	ma_uint32 nplay = 0, ncap = 0;
	if (ma_context_get_devices(&ctx, &play, &nplay, &cap, &ncap)
	    != MA_SUCCESS) {
		fprintf(stderr, "audio: cannot enumerate devices\n");
		ma_context_uninit(&ctx);
		return 0;
	}

	const ma_device_info *list = (dir == ARDOP_AUDIO_CAPTURE) ? cap : play;
	size_t n = (dir == ARDOP_AUDIO_CAPTURE) ? ncap : nplay;
	if (n > max)
		n = max;

	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		ardop_ma_id_to_str(&list[i].id, ctx.backend, out[i].id,
				   sizeof(out[i].id));
		snprintf(out[i].name, sizeof(out[i].name), "%s", list[i].name);
		out[i].is_default = list[i].isDefault ? true : false;
	}

	ma_context_uninit(&ctx);
	return n;
}

bool ardop_audio_print_devices(const char *backend_name)
{
	static ardop_audio_device devs[64];

	for (int d = 0; d < 2; d++) {
		ardop_audio_dir dir = d ? ARDOP_AUDIO_PLAYBACK
					: ARDOP_AUDIO_CAPTURE;
		size_t n = ardop_audio_enumerate(dir, devs,
						 sizeof(devs) / sizeof(devs[0]),
						 backend_name);
		fprintf(stderr, "\n%s devices:\n",
			d ? "Playback" : "Capture");
		if (n == 0) {
			fprintf(stderr, "  (none found)\n");
			continue;
		}
		for (size_t i = 0; i < n; i++)
			fprintf(stderr, "  %s%s\n      id: %s\n",
				devs[i].name,
				devs[i].is_default ? "  [default]" : "",
				devs[i].id);
	}
	fprintf(stderr,
		"\nPass an id or a name to --audio CAPTURE PLAYBACK. Both are\n"
		"matched, id first, so a saved selection survives a device\n"
		"being renumbered by a replug.\n");
	return true;
}
