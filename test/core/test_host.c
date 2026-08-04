#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/build.h"
#include "shell/host.h"
#include "shell/runtime.h"
#include "codec/stationid.h"

/*
 * The host command protocol under test: each command line is fed to
 * ardop_host_command over a runtime, and the reply string (the frozen wire
 * format Pat/WoAD depend on) plus the resulting runtime state are checked. No
 * socket -- the transport is separable; this is the whole command surface.
 */

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

static ardop_runtime g_rt;
static char g_reply[1024];

static void reset(void)
{
	assert_true(ardop_runtime_init(&g_rt, kRSLens, NUM_RSLENS));
	g_rt.link.bw_setting = ARDOP_ARQ_BW_2000_MAX;
}

/* Feed one command line; return the reply. */
static const char *cmd(const char *line)
{
	ardop_host_command(&g_rt, line, 0, g_reply, sizeof(g_reply));
	return g_reply;
}

static void test_host_config_queries_and_sets(void **state)
{
	(void)state;
	reset();

	/* MYCALL: query empty, set, echo, query. */
	assert_string_equal(cmd("MYCALL"), "MYCALL ");
	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");
	assert_string_equal(cmd("MYCALL"), "MYCALL N0AAA");
	assert_int_equal(g_rt.link.mycall.str[0], 'N');

	/* Lowercase input is uppercased like the inherited TNC. */
	assert_string_equal(cmd("mycall n0bbb"), "MYCALL now N0BBB");

	/* Invalid callsign -> FAULT, unchanged. */
	assert_true(strncmp(cmd("MYCALL not-a-call"),
			    "FAULT Syntax Err: MYCALL NOT-A-CALL:", 35) == 0);
	assert_string_equal(g_rt.link.mycall.str, "N0BBB");

	/* ARQBW: query, set, invalid. */
	assert_string_equal(cmd("ARQBW"), "ARQBW 2000MAX");
	assert_string_equal(cmd("ARQBW 500FORCED"), "ARQBW now 500FORCED");
	assert_int_equal(g_rt.link.bw_setting, ARDOP_ARQ_BW_500_FORCED);
	assert_string_equal(cmd("ARQBW 999"), "FAULT Syntax Err: ARQBW 999");

	/* GRIDSQUARE. Input is uppercased on the way in; the locator's canonical
	 * form lowercases the sub-square letters. */
	assert_string_equal(cmd("GRIDSQUARE FN31pr"), "GRIDSQUARE now FN31pr");

	/* Booleans. */
	assert_string_equal(cmd("LISTEN"), "LISTEN FALSE");
	assert_string_equal(cmd("LISTEN TRUE"), "LISTEN now TRUE");
	assert_true(g_rt.link.listening);
	assert_string_equal(cmd("LISTEN MAYBE"),
			    "FAULT Syntax Err: LISTEN MAYBE");
	assert_string_equal(cmd("AUTOBREAK TRUE"), "AUTOBREAK now TRUE");
	assert_true(g_rt.link.auto_break);

	/* PROTOCOLMODE. */
	assert_string_equal(cmd("PROTOCOLMODE"), "PROTOCOLMODE ARQ");
	assert_string_equal(cmd("PROTOCOLMODE FEC"), "PROTOCOLMODE now FEC");
	assert_int_equal(g_rt.link.mode, ARDOP_MODE_FEC);
	assert_string_equal(cmd("PROTOCOLMODE ARQ"), "PROTOCOLMODE now ARQ");

	/* FECMODE: the default is the inherited 4PSK.200.100, and a set is
	 * echoed back by name. */
	assert_string_equal(cmd("FECMODE"), "FECMODE 4PSK.200.100");
	assert_string_equal(cmd("FECMODE 8PSK.1000.100"),
			    "FECMODE now 8PSK.1000.100");
	assert_int_equal(g_rt.link.fec_frame_type, 0x62);
	assert_string_equal(cmd("FECMODE"), "FECMODE 8PSK.1000.100");

	/* The short "S" variants are distinct names, not prefixes of each
	 * other, and an unknown name leaves the setting alone. */
	assert_string_equal(cmd("FECMODE 4PSK.200.100S"),
			    "FECMODE now 4PSK.200.100S");
	assert_int_equal(g_rt.link.fec_frame_type, 0x42);
	assert_string_equal(cmd("FECMODE 4PSK.200.100"),
			    "FECMODE now 4PSK.200.100");
	assert_int_equal(g_rt.link.fec_frame_type, 0x40);
	assert_string_equal(cmd("FECMODE 9PSK.200.100"),
			    "FAULT Syntax Err: FECMODE 9PSK.200.100");
	assert_int_equal(g_rt.link.fec_frame_type, 0x40);

	/* A control frame is not a FECMODE, even though it names a frame. */
	assert_string_equal(cmd("FECMODE IDFRAME"),
			    "FAULT Syntax Err: FECMODE IDFRAME");

	/* FECREPEATS bounds. */
	assert_string_equal(cmd("FECREPEATS 3"), "FECREPEATS now 3");
	assert_int_equal(g_rt.link.fec_repeats, 3);
	assert_string_equal(cmd("FECREPEATS 9"), "FAULT Syntax Err: FECREPEATS 9");

	/* MYAUX round-trips a list. */
	assert_string_equal(cmd("MYAUX N0CCC,N0DDD"), "MYAUX now N0CCC,N0DDD");
	assert_int_equal(g_rt.link.n_aux, 2);
	assert_string_equal(cmd("MYAUX"), "MYAUX N0CCC,N0DDD");
}

static void test_host_state_and_version(void **state)
{
	(void)state;
	reset();

	assert_string_equal(cmd("STATE"), "STATE DISC");
	assert_string_equal(cmd("VERSION"),
			    "VERSION " ARDOP_HOST_PRODUCT "_" ARDOP_HOST_VERSION);

	/*
	 * And the literal, because this string leaves the program.
	 *
	 * A host program such as Pat reads this reply. The line above is built
	 * from the same macros as the code, so it passes whatever the macros
	 * say; only a literal makes a change visible. The name was changed from
	 * `ardopcf` to `ardopb` on purpose -- see host.h -- and the next change
	 * must be as deliberate as that one was.
	 */
	assert_string_equal(cmd("VERSION"), "VERSION ardopb_1.0.4.1.3-b");
	assert_string_equal(cmd("STATE EXTRA"), "FAULT Syntax Err: STATE EXTRA");
	assert_string_equal(cmd("FLIBBLE"), "FAULT CMD FLIBBLE not recoginized");
	assert_string_equal(cmd("RDY"), "");
}

/*
 * The build identifier.
 *
 * Not a protocol value and not tested for its content -- it changes with every
 * commit. What matters is that it is never empty and never NULL, because a
 * fault report with a blank build field cannot be told from one where nobody
 * filled the field in.
 */
static void test_build_identity(void **state)
{
	(void)state;

	assert_non_null(ardop_build_id());
	assert_true(ardop_build_id()[0] != '\0');
	assert_non_null(ardop_build_date());
	assert_true(ardop_build_date()[0] != '\0');

	char line[160];
	assert_ptr_equal(ardop_build_line("ardopb", line, sizeof line), line);
	assert_non_null(strstr(line, "ardopb"));
	assert_non_null(strstr(line, ardop_build_id()));
	/* The protocol version travels with it, so one line answers both
	 * "which build" and "which protocol". */
	assert_non_null(strstr(line, ARDOP_HOST_VERSION));

	/* A short buffer truncates and does not overrun. */
	char small[8];
	ardop_build_line("ardopb", small, sizeof small);
	assert_true(strlen(small) < sizeof small);
}

static void test_host_actions_guarded_by_mycall(void **state)
{
	(void)state;
	reset();

	/* Actions that transmit need MYCALL first. */
	assert_string_equal(cmd("SENDID"), "FAULT MYCALL not set");
	assert_string_equal(cmd("ARQCALL N0BBB 5"), "FAULT MYCALL not set");

	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");

	/* SENDID from DISC starts a transmission (the runtime keys TX). */
	assert_string_equal(cmd("SENDID"), "SENDID");
	assert_true(ardop_runtime_tx_active(&g_rt));

	/* DISCONNECT while disconnected is ignored. */
	reset();
	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");
	assert_string_equal(cmd("DISCONNECT"), "DISCONNECT IGNORED");

	/* ABORT always acknowledges. */
	assert_string_equal(cmd("ABORT"), "ABORT");
}

static void test_host_arqcall_connects(void **state)
{
	(void)state;
	reset();

	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");
	/* The reply echoes the original-case command; the runtime keys the
	 * ConReq. */
	assert_string_equal(cmd("ARQCALL n0bbb 3"), "ARQCALL n0bbb 3");
	assert_true(ardop_runtime_tx_active(&g_rt));
	assert_int_equal(g_rt.link.state, ARDOP_LINK_ISS_CON_REQ);

	/* Bad target -> FAULT, no connect. */
	reset();
	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");
	const char *r = cmd("ARQCALL bad-call 3");
	assert_true(strncmp(r, "FAULT Syntax Err: ARQCALL", 25) == 0);
	assert_int_equal(g_rt.link.state, ARDOP_LINK_DISC);
}

/*
 * FECSEND broadcasts the queued buffer without the host having to name a
 * FECMODE first. Regression: the frame type defaulted to 0x00, which carries no
 * payload, so the link entered FECSEND, found a zero-capacity frame, and
 * silently returned to DISC -- the host saw "FECSEND now TRUE" and a buffer
 * that never drained.
 */
static void test_host_fecsend_uses_default_fecmode(void **state)
{
	(void)state;
	reset();

	assert_string_equal(cmd("MYCALL N0AAA"), "MYCALL now N0AAA");
	assert_string_equal(cmd("PROTOCOLMODE FEC"), "PROTOCOLMODE now FEC");

	/* Nothing queued: FECSEND has nothing to broadcast. */
	assert_string_equal(cmd("FECSEND TRUE"),
			    "FAULT StartFEC failed for FECSEND TRUE.");

	const uint8_t payload[] = "hello world";
	ardop_host_cmd sd = {.kind = ARDOP_CMD_SEND_DATA, .data = payload,
			     .data_len = sizeof(payload) - 1};
	ardop_runtime_host(&g_rt, &sd, 0);
	assert_int_equal(g_rt.link.tx_len, sizeof(payload) - 1);

	/* Now it transmits: TX is keyed and the buffer is consumed (FEC has no
	 * ACKs, so the bytes leave the queue as they are sent). */
	assert_string_equal(cmd("FECSEND TRUE"), "FECSEND now TRUE");
	assert_true(ardop_runtime_tx_active(&g_rt));
	assert_int_equal(g_rt.link.state, ARDOP_LINK_FEC_SEND);
	assert_int_equal(g_rt.link.tx_len, 0);
}

/* The binary data channel: TNC->host framing and host->TNC parsing. */
static void test_host_data_framing(void **state)
{
	(void)state;

	/* TNC->host: <2-byte BE (3+len)><tag><payload>. */
	const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	uint8_t out[64];
	size_t n = ardop_host_data_frame(out, sizeof(out), "ARQ", payload, 4);
	assert_int_equal(n, 2 + 3 + 4);
	assert_int_equal(out[0], 0);        /* (3+4) >> 8 */
	assert_int_equal(out[1], 7);        /* (3+4) & 0xFF */
	assert_memory_equal(out + 2, "ARQ", 3);
	assert_memory_equal(out + 5, payload, 4);

	/* A too-small buffer refuses rather than overruns. */
	assert_int_equal(ardop_host_data_frame(out, 5, "FEC", payload, 4), 0);

	/* host->TNC: <2-byte BE len><payload>. Parse a message plus a trailing
	 * partial one from a single buffer. */
	uint8_t stream[] = {
		0x00, 0x03, 'a', 'b', 'c',   /* complete: len 3 "abc" */
		0x00, 0x05, 'x', 'y',        /* partial: len 5, only 2 present */
	};
	const uint8_t *p;
	size_t plen, consumed;
	assert_true(ardop_host_data_parse(stream, sizeof(stream), &p, &plen,
					  &consumed));
	assert_int_equal(plen, 3);
	assert_memory_equal(p, "abc", 3);
	assert_int_equal(consumed, 5);

	/* After consuming the first, the remainder is incomplete. */
	assert_false(ardop_host_data_parse(stream + consumed,
					   sizeof(stream) - consumed, &p, &plen,
					   &consumed));

	/* A length prefix alone (no payload yet) is incomplete. */
	uint8_t just_len[] = {0x00, 0x04};
	assert_false(ardop_host_data_parse(just_len, 2, &p, &plen, &consumed));

	/* A zero-length payload is a complete, empty message. */
	uint8_t empty[] = {0x00, 0x00};
	assert_true(ardop_host_data_parse(empty, 2, &p, &plen, &consumed));
	assert_int_equal(plen, 0);
	assert_int_equal(consumed, 2);
}

/* Data arriving on the (parsed) data channel queues into the runtime's TX
 * buffer via a SEND_DATA link input -- the path the socket layer drives. */
static void test_host_data_queues_to_runtime(void **state)
{
	(void)state;
	reset();

	uint8_t stream[] = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
	const uint8_t *p;
	size_t plen, consumed;
	assert_true(ardop_host_data_parse(stream, sizeof(stream), &p, &plen,
					  &consumed));

	ardop_host_cmd sd = {.kind = ARDOP_CMD_SEND_DATA, .data = p,
			     .data_len = plen};
	ardop_runtime_host(&g_rt, &sd, 0);
	assert_int_equal(g_rt.link.tx_len, 5);
	assert_memory_equal(g_rt.link.tx_data, "hello", 5);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_host_config_queries_and_sets),
		cmocka_unit_test(test_host_state_and_version),
		cmocka_unit_test(test_build_identity),
		cmocka_unit_test(test_host_actions_guarded_by_mycall),
		cmocka_unit_test(test_host_arqcall_connects),
		cmocka_unit_test(test_host_fecsend_uses_default_fecmode),
		cmocka_unit_test(test_host_data_framing),
		cmocka_unit_test(test_host_data_queues_to_runtime),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
