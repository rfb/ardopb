#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

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
	assert_string_equal(cmd("STATE EXTRA"), "FAULT Syntax Err: STATE EXTRA");
	assert_string_equal(cmd("FLIBBLE"), "FAULT CMD FLIBBLE not recoginized");
	assert_string_equal(cmd("RDY"), "");
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

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_host_config_queries_and_sets),
		cmocka_unit_test(test_host_state_and_version),
		cmocka_unit_test(test_host_actions_guarded_by_mycall),
		cmocka_unit_test(test_host_arqcall_connects),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
