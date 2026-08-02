#include "shell/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/sys.h"

/**
 * @file settings.c
 * @brief The key/value store (see settings.h).
 *
 * Portable C11 with no OS header: the platform half -- where configuration
 * lives, how a directory is made, how a file is atomically replaced -- is in
 * `shell/sys.h`. Keeping the parser separate from the environment is what lets
 * every case below be tested against a caller-supplied path.
 */

#define APP_NAME "ardop"
#define FILE_NAME "station.conf"

/* --- small helpers --------------------------------------------------------- */

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trim in place, returning the start. */
static char *trim(char *s)
{
	while (*s && is_space(*s))
		s++;
	size_t n = strlen(s);
	while (n > 0 && is_space(s[n - 1]))
		s[--n] = '\0';
	return s;
}

static bool eq_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (x != y)
			return false;
	}
	return *a == *b;
}

/* Index of @p key, or s->n when absent. An index rather than a pointer so the
 * const and non-const callers share one lookup without casting the qualifier
 * away. */
static size_t find(const ardop_settings *s, const char *key)
{
	size_t i = 0;
	for (; i < s->n; i++)
		if (strcmp(s->item[i].key, key) == 0)
			break;
	return i;
}

/* --- path ------------------------------------------------------------------ */

bool ardop_settings_path(char *out, size_t cap)
{
	char dir[400];
	if (!ardop_config_dir(APP_NAME, dir, sizeof dir)) {
		if (cap)
			out[0] = '\0';
		return false;
	}
#ifdef _WIN32
	const char *sep = "\\";
#else
	const char *sep = "/";
#endif
	int n = snprintf(out, cap, "%s%s%s", dir, sep, FILE_NAME);
	if (n < 0 || (size_t)n >= cap) {
		if (cap)
			out[0] = '\0';
		return false;
	}
	return true;
}

/* --- load / save ----------------------------------------------------------- */

bool ardop_settings_load(ardop_settings *s, const char *path)
{
	memset(s, 0, sizeof *s);

	FILE *f = fopen(path, "r");
	if (!f) {
		/* First run. An application that treats "you have never
		 * configured me" as an error is an application nobody gets past. */
		return true;
	}

	char line[ARDOP_SETTING_KEY_MAX + ARDOP_SETTING_VALUE_MAX + 8];
	while (fgets(line, sizeof line, f)) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';

		char *body = trim(line);
		if (*body == '\0')
			continue;

		char *eq = strchr(body, '=');
		if (!eq) {
			s->skipped++;
			continue;
		}
		*eq = '\0';

		char *key = trim(body);
		char *value = trim(eq + 1);
		if (*key == '\0' || strlen(key) >= ARDOP_SETTING_KEY_MAX ||
		    strlen(value) >= ARDOP_SETTING_VALUE_MAX) {
			s->skipped++;
			continue;
		}

		if (!ardop_settings_set(s, key, value))
			s->truncated = true;
	}

	fclose(f);
	return true;
}

bool ardop_settings_save(const ardop_settings *s, const char *path)
{
	/* Refuse rather than lose keys. A store that could not hold everything
	 * the file contained would write back a shorter file, deleting whatever
	 * did not fit -- silently, and probably somebody else's settings. */
	if (s->truncated)
		return false;

	char tmp[512];
	int n = snprintf(tmp, sizeof tmp, "%s.new", path);
	if (n < 0 || (size_t)n >= sizeof tmp)
		return false;

	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;

	bool ok = fprintf(f,
		"# ardop station settings. Written by the application;\n"
		"# safe to edit by hand. Unrecognised keys are preserved.\n") > 0;

	for (size_t i = 0; i < s->n && ok; i++)
		ok = fprintf(f, "%s=%s\n", s->item[i].key, s->item[i].value) > 0;

	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		remove(tmp);
		return false;
	}

	if (!ardop_replace_file(tmp, path)) {
		remove(tmp);
		return false;
	}
	return true;
}

/* --- get / set ------------------------------------------------------------- */

const char *ardop_settings_get(const ardop_settings *s, const char *key,
			       const char *fallback)
{
	size_t i = find(s, key);
	return i < s->n ? s->item[i].value : fallback;
}

long ardop_settings_get_num(const ardop_settings *s, const char *key, long fb)
{
	const char *v = ardop_settings_get(s, key, NULL);
	if (!v || !*v)
		return fb;

	char *end = NULL;
	long out = strtol(v, &end, 10);
	return (end && *end == '\0') ? out : fb;
}

bool ardop_settings_get_bool(const ardop_settings *s, const char *key, bool fb)
{
	const char *v = ardop_settings_get(s, key, NULL);
	if (!v || !*v)
		return fb;
	if (eq_nocase(v, "true") || eq_nocase(v, "yes") || eq_nocase(v, "on") ||
	    strcmp(v, "1") == 0)
		return true;
	if (eq_nocase(v, "false") || eq_nocase(v, "no") || eq_nocase(v, "off") ||
	    strcmp(v, "0") == 0)
		return false;
	return fb;
}

bool ardop_settings_set(ardop_settings *s, const char *key, const char *value)
{
	if (!key || !*key || !value)
		return false;
	if (strlen(key) >= ARDOP_SETTING_KEY_MAX ||
	    strlen(value) >= ARDOP_SETTING_VALUE_MAX)
		return false;

	size_t i = find(s, key);
	if (i == s->n) {
		if (s->n >= ARDOP_SETTINGS_MAX)
			return false;
		s->n++;
		snprintf(s->item[i].key, sizeof s->item[i].key, "%s", key);
	}
	snprintf(s->item[i].value, sizeof s->item[i].value, "%s", value);
	return true;
}

bool ardop_settings_set_num(ardop_settings *s, const char *key, long value)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%ld", value);
	return ardop_settings_set(s, key, buf);
}

bool ardop_settings_set_bool(ardop_settings *s, const char *key, bool value)
{
	return ardop_settings_set(s, key, value ? "true" : "false");
}

bool ardop_settings_unset(ardop_settings *s, const char *key)
{
	for (size_t i = 0; i < s->n; i++) {
		if (strcmp(s->item[i].key, key) != 0)
			continue;
		/* Order is not meaningful, so move the last one down rather than
		 * shuffling everything. */
		s->item[i] = s->item[s->n - 1];
		s->n--;
		return true;
	}
	return false;
}
