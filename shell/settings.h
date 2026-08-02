#ifndef ARDOP_SHELL_SETTINGS_H_
#define ARDOP_SHELL_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * @file settings.h
 * @brief A small persistent key/value store.
 *
 * Everything in this tree has been argv-only until now, which is fine for a
 * daemon someone starts from a script and useless for an application: a device
 * selection that does not survive a restart is not a feature. This is where it
 * survives.
 *
 * ## The format
 *
 * `key=value`, one per line, UTF-8, `#` to end of line for comments. Whitespace
 * around the key and around the value is stripped; the value is otherwise
 * literal. There is **no quoting and no escaping** -- every value this needs to
 * carry (device ids, paths, callsigns, `true`/`false`, integers) is a single
 * line with no leading or trailing space, and inventing an escape syntax now
 * would be a schema nobody has asked for.
 *
 * ## The one rule that makes it extensible: unknown keys are preserved
 *
 * ::ardop_settings_load reads everything it finds; ::ardop_settings_save writes
 * everything back. So a later part of the program adds a key by calling
 * ::ardop_settings_set and changing nothing else -- no registry, no schema, no
 * migration framework -- and a file written by a newer build survives being
 * loaded and saved by an older one intact.
 *
 * Comments and ordering are *not* preserved. This is a store, not a document;
 * pretending otherwise would need a line model and a reason.
 *
 * Namespaces are by dotted prefix, and ownership is by convention: `audio.*`
 * and `ptt.*` belong to the device manager, `station.*`, `link.*` and `ui.*` to
 * the application above it. Nothing arbitrates, and nothing needs to.
 *
 * House style: caller-owned storage, fixed capacity, no globals, no OS headers.
 * Paths come from `shell/sys.h`.
 */

#define ARDOP_SETTING_KEY_MAX   64

/** @brief Enough for an ::ARDOP_DEV_ID_MAX device id with room to spare. */
#define ARDOP_SETTING_VALUE_MAX 512

#define ARDOP_SETTINGS_MAX      128

typedef struct {
	char key[ARDOP_SETTING_KEY_MAX];
	char value[ARDOP_SETTING_VALUE_MAX];
} ardop_setting;

/** @brief The store. Zero-initialise, or ::ardop_settings_load into it. */
typedef struct {
	ardop_setting item[ARDOP_SETTINGS_MAX];
	size_t n;

	/**
	 * @brief Unparseable lines skipped by the last load.
	 *
	 * A malformed line does not abort the load: one bad hand edit must not
	 * cost an operator their callsign. It is counted so a caller can say so.
	 */
	size_t skipped;

	/**
	 * @brief The file held more keys than fit, so ::ardop_settings_save
	 *        refuses.
	 *
	 * Writing back a store that could not hold what it read would silently
	 * delete the keys that did not fit.
	 */
	bool truncated;
} ardop_settings;

/**
 * @brief The default settings path, creating its directory.
 *
 * `$XDG_CONFIG_HOME/ardop/station.conf` (or `$HOME/.config/...`) on POSIX,
 * `%%APPDATA%%\ardop\station.conf` on Windows.
 *
 * @return false if no configuration directory can be determined or created.
 */
bool ardop_settings_path(char *out, size_t cap);

/**
 * @brief Load from @p path.
 *
 * **A missing file is success**, with an empty store: first run is not an error.
 *
 * @return false only if the file exists and cannot be read.
 */
bool ardop_settings_load(ardop_settings *s, const char *path);

/**
 * @brief Write @p s to @p path.
 *
 * Atomic: writes `path.new` and then replaces, so an interrupted save leaves
 * either the old file or the new one and never an empty one.
 *
 * @return false if @ref ardop_settings::truncated is set, or on an I/O error.
 */
bool ardop_settings_save(const ardop_settings *s, const char *path);

/** @brief The value for @p key, or @p fallback. Never NULL if @p fallback is not. */
const char *ardop_settings_get(const ardop_settings *s, const char *key,
			       const char *fallback);
long ardop_settings_get_num(const ardop_settings *s, const char *key, long fb);

/** @brief Reads "true"/"yes"/"on"/"1" as true, case-insensitively. */
bool ardop_settings_get_bool(const ardop_settings *s, const char *key, bool fb);

/**
 * @brief Set @p key to @p value, replacing any existing entry.
 * @return false if the key or value is too long, or the store is full.
 */
bool ardop_settings_set(ardop_settings *s, const char *key, const char *value);
bool ardop_settings_set_num(ardop_settings *s, const char *key, long value);
bool ardop_settings_set_bool(ardop_settings *s, const char *key, bool value);

/** @brief Remove @p key. @return true if it was there. */
bool ardop_settings_unset(ardop_settings *s, const char *key);

#endif /* ARDOP_SHELL_SETTINGS_H_ */
