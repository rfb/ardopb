#include "shell/audio_devices.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "shell/ma_common.h"
#include "shell/resample.h"
#include "modem/modulate.h"

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

/* --- the resolution rule --------------------------------------------------- */

/*
 * Pure, and deliberately so. This used to be a static function inside
 * backend_ma.c that warned on stderr and fell back to the default -- which meant
 * a settings screen could not reuse it, a test could not reach it, and an
 * operator running a graphical program never saw the warning at all. The rule is
 * the same; what changed is that the answer is returned instead of printed.
 */
ardop_audio_match ardop_audio_match_device(const ardop_audio_device *devs,
					   size_t n, const char *want_id,
					   const char *want_name, size_t *index)
{
	bool has_id = want_id && *want_id;
	bool has_name = want_name && *want_name;

	/* Pass 1: the id the operator's device had when they chose it. */
	if (has_id)
		for (size_t i = 0; i < n; i++)
			if (strcmp(devs[i].id, want_id) == 0) {
				*index = i;
				return ARDOP_AUDIO_MATCH_ID;
			}

	/* Pass 2: the name. An index renumbered by a replug lands here, which is
	 * the whole reason both fields are persisted. */
	if (has_name)
		for (size_t i = 0; i < n; i++)
			if (strcmp(devs[i].name, want_name) == 0) {
				*index = i;
				return ARDOP_AUDIO_MATCH_NAME;
			}

	for (size_t i = 0; i < n; i++)
		if (devs[i].is_default) {
			*index = i;
			return ARDOP_AUDIO_MATCH_DEFAULT;
		}

	/* No default advertised. Falling back to the first device would be a
	 * guess about which piece of hardware to transmit through. */
	if (n > 0 && !has_id && !has_name) {
		*index = 0;
		return ARDOP_AUDIO_MATCH_DEFAULT;
	}
	return ARDOP_AUDIO_MATCH_NONE;
}

const char *ardop_audio_match_str(ardop_audio_match m)
{
	switch (m) {
	case ARDOP_AUDIO_MATCH_ID:      return "the selected device";
	case ARDOP_AUDIO_MATCH_NAME:    return "matched by name; its id has changed";
	case ARDOP_AUDIO_MATCH_DEFAULT: return "the system default";
	case ARDOP_AUDIO_MATCH_NONE:    return "no device available";
	}
	return "unknown";
}

bool ardop_audio_match_is_substitution(ardop_audio_match m)
{
	/* A name match is not a substitution: it is the operator's device, found
	 * after a renumber, and the id is simply rewritten. Only landing on the
	 * default -- or on nothing -- means they did not get what they asked for. */
	return m == ARDOP_AUDIO_MATCH_DEFAULT || m == ARDOP_AUDIO_MATCH_NONE;
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

	ma_device_type type = (dir == ARDOP_AUDIO_CAPTURE) ? ma_device_type_capture
							  : ma_device_type_playback;

	for (size_t i = 0; i < n; i++) {
		memset(&out[i], 0, sizeof(out[i]));
		ardop_ma_id_to_str(&list[i].id, ctx.backend, out[i].id,
				   sizeof(out[i].id));
		snprintf(out[i].name, sizeof(out[i].name), "%s", list[i].name);
		out[i].is_default = list[i].isDefault ? true : false;

		/* The rate, so a settings screen can say "44100 Hz -- cannot be
		 * used" before the operator picks it and gets a refusal from the
		 * open. One extra call per device, which is most of why this
		 * function is documented as slow and off the modem thread.
		 *
		 * A device reports a *list* of native formats, not one rate, so
		 * prefer any entry we could actually run at and fall back to the
		 * first for display. */
		ma_device_info info;
		if (ma_context_get_device_info(&ctx, type, &list[i].id, &info)
		    != MA_SUCCESS)
			continue;

		for (ma_uint32 k = 0; k < info.nativeDataFormatCount; k++) {
			unsigned rate = info.nativeDataFormats[k].sampleRate;
			if (rate == 0)
				continue;
			bool ok = rate % ARDOP_MOD_SAMPLE_RATE == 0 &&
				  rate / ARDOP_MOD_SAMPLE_RATE
					  <= ARDOP_RESAMPLE_MAX_M;
			if (ok && !out[i].rate_ok) {
				out[i].native_rate = rate;
				out[i].rate_ok = true;
			} else if (!out[i].native_rate) {
				out[i].native_rate = rate;
			}
		}
	}

	ma_context_uninit(&ctx);
	return n;
}

ardop_audio_match ardop_audio_resolve(ardop_audio_dir dir, const char *want_id,
				      const char *want_name,
				      const char *backend_name,
				      ardop_audio_device *out)
{
	static ardop_audio_device devs[64];

	memset(out, 0, sizeof(*out));
	size_t n = ardop_audio_enumerate(dir, devs,
					 sizeof(devs) / sizeof(devs[0]),
					 backend_name);
	size_t idx = 0;
	ardop_audio_match m = ardop_audio_match_device(devs, n, want_id,
						       want_name, &idx);
	if (m != ARDOP_AUDIO_MATCH_NONE)
		*out = devs[idx];
	return m;
}

bool ardop_audio_print_devices(const char *backend_name)
{
	static ardop_audio_device devs[64];
	size_t found = 0, usable = 0;

	for (int d = 0; d < 2; d++) {
		ardop_audio_dir dir = d ? ARDOP_AUDIO_PLAYBACK
					: ARDOP_AUDIO_CAPTURE;
		size_t n = ardop_audio_enumerate(dir, devs,
						 sizeof(devs) / sizeof(devs[0]),
						 backend_name);
		printf("\n%s devices:\n", d ? "Playback" : "Capture");
		if (n == 0) {
			printf("  (none found)\n");
			continue;
		}
		found += n;
		for (size_t i = 0; i < n; i++) {
			if (devs[i].rate_ok)
				usable++;
			char rate[48];
			if (devs[i].rate_ok)
				snprintf(rate, sizeof rate, "%u Hz",
					 devs[i].native_rate);
			else if (devs[i].native_rate)
				snprintf(rate, sizeof rate,
					 "%u Hz -- cannot be used",
					 devs[i].native_rate);
			else
				snprintf(rate, sizeof rate,
					 "rate not known until opened");

			printf("  %s%s\n      id:   %s\n      rate: %s\n",
			       devs[i].name,
			       devs[i].is_default ? "  [default]" : "",
			       devs[i].id, rate);
		}
	}
	printf("\nPass an id or a name to --audio CAPTURE PLAYBACK. Both are\n"
	       "matched, id first, so a saved selection survives a device\n"
	       "being renumbered by a replug.\n");

	/*
	 * Saying it once, at the end, rather than leaving an operator to notice
	 * that every line said "cannot be used". A machine where nothing is
	 * usable is usually a sound server defaulting to 44100 rather than a
	 * machine with no suitable hardware, and that is fixable in one setting.
	 */
	if (found > 0 && usable == 0) {
		printf("\nNone of these can be used: the modem needs a whole "
		       "multiple of 12000 Hz\n"
		       "(12000, 24000, 48000 or 96000), and every device above "
		       "reports something\n"
		       "else. That is usually the sound server's default rate "
		       "rather than the\n"
		       "hardware -- setting it to 48000 fixes every device at "
		       "once:\n"
		       "\n"
#ifdef _WIN32
		       "  Sound Control Panel -> the device -> Properties -> "
		       "Advanced -> 48000 Hz\n");
#else
		       "  PipeWire:   ~/.config/pipewire/pipewire.conf.d/"
		       "10-rate.conf\n"
		       "              context.properties = "
		       "{ default.clock.rate = 48000 }\n"
		       "  PulseAudio: /etc/pulse/daemon.conf, "
		       "default-sample-rate = 48000, then pulseaudio -k\n"
		       "  Or bypass the sound server: --audio-backend alsa\n");
#endif
	}

	return found > 0;
}
