#ifndef ARDOP_SHELL_BUILD_H_
#define ARDOP_SHELL_BUILD_H_

/**
 * @file build.h
 * @brief Which build of this program is running.
 *
 * A tester reports a fault. The first question is always the same: which build?
 * Before this, the answer was in a `VERSION` file beside the program, and only
 * in the release package. A person who built the program, or who moved the
 * executable, had no answer at all.
 *
 * ## One identity, not two
 *
 * The host command `VERSION` reports this same value, as
 * `VERSION ardopb_0f35a4d`. There used to be a second number there -- an
 * inherited `ardopcf` release -- and it named software this is not. See
 * `host.h` for why it was dropped.
 *
 * So the string a host program records, the string in a fault report, and the
 * string that finds the source are all one string.
 */

/**
 * @brief The build identifier.
 *
 * The output of `git describe --tags --always --dirty` at build time. Examples:
 *
 * - `v0.4-12-g4e85005` -- 12 commits after the tag `v0.4`.
 * - `4e85005-dirty` -- no tag, and the tree had local changes.
 * - `unknown` -- the build had no git repository. A release tarball gives this.
 *
 * The suffix `-dirty` is important. It means the source was not the source in
 * the repository, so a fault report from that build is not reproducible from a
 * commit.
 *
 * @return A static string. Never NULL and never empty.
 */
const char *ardop_build_id(void);

/**
 * @brief The build date, as `YYYY-MM-DD`.
 *
 * Included because `ardop_build_id` can be `unknown`, and a date is then the
 * only remaining evidence of which build a person has.
 */
const char *ardop_build_date(void);

/**
 * @brief One line for a log, a title bar or a fault report.
 *
 * Format: `ardopb 0f35a4d-dirty (2026-08-04)`.
 *
 * @param program The program name, for example `ardopb`.
 * @param out     The caller's buffer.
 * @param cap     The size of @p out.
 * @return @p out.
 */
const char *ardop_build_line(const char *program, char *out, unsigned long cap);

#endif /* ARDOP_SHELL_BUILD_H_ */
