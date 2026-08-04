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
 * ## This is not the host protocol version
 *
 * `ARDOP_HOST_VERSION` in `host.h` is `1.0.4.1.3-b`, and the host command
 * `VERSION` reports it as `ardopcf_1.0.4.1.3-b`. That value tells a host program
 * such as Pat which `ardopcf` this modem is compatible with. **Do not change
 * it.** It describes the protocol, not this software.
 *
 * The two values answer two different questions:
 *
 * | Question | Answer |
 * |---|---|
 * | Which protocol do you speak? | ::ARDOP_HOST_VERSION |
 * | Which build are you? | ::ardop_build_id |
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
 * Format: `ardopb 4e85005-dirty (2026-08-03), protocol ardopcf_1.0.4.1.3-b`.
 *
 * @param program The program name, for example `ardopb`.
 * @param out     The caller's buffer.
 * @param cap     The size of @p out.
 * @return @p out.
 */
const char *ardop_build_line(const char *program, char *out, unsigned long cap);

#endif /* ARDOP_SHELL_BUILD_H_ */
