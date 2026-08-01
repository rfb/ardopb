#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/loopback.h"
#include "app/spine.h"

/**
 * @file stress_spine.c
 * @brief Two-thread ThreadSanitizer stress for the embedding spine.
 *
 * The rule the whole design rests on is
 * [analysis/14](../../analysis/14-station-application.md) Decision 2:
 *
 *   > One runtime, one thread. Nothing outside the modem thread touches
 *   > ardop_runtime or anything reachable from it, for any reason, ever.
 *
 * That is a claim about what does *not* happen, so no single-threaded test can
 * support it. `test/core/test_spine.c` proves the rings and the credit behave; it
 * cannot prove that a user interface hammering the seam never reaches the
 * demodulator. This does: one thread runs a full ARQ session while another
 * submits, drains and polls as fast as it can, with ThreadSanitizer watching.
 *
 * Three things make the result mean something:
 *
 *   - `make test-app-tsan` compiles the *sources* rather than linking the
 *     objects. The sanitizer only sees races in instrumented code, and the point
 *     is precisely that `core/` and `shell/runtime.c` are never touched from the
 *     second thread -- so they have to be instrumented for their silence to be
 *     evidence.
 *   - `app_spine::tx_offered` and `tx_applied` are deliberately plain, not
 *     atomic. Each belongs to exactly one thread; making them atomic would have
 *     hidden a violation of that ownership instead of reporting it here.
 *   - The payload is checked byte for byte, so a torn record fails the test even
 *     in a build without the sanitizer.
 *
 * MinGW has no ThreadSanitizer, but nothing under test is platform-specific, so
 * proving it on Linux proves it everywhere -- the argument `check-pure` makes.
 */

#define PAYLOAD 32768u
#define SAMPLE_RATE 12000u

/* Give up rather than hang: a wedged link would otherwise burn the whole CI
 * job's timeout before saying anything. Ten minutes of simulated air is far more
 * than this transfer needs. */
#define DEADLINE_SAMPLES (600u * SAMPLE_RATE)

static app_loopback *lb;
static app_spine *a, *b;

/* Set by the modem thread when the session is over or has gone wrong; read by
 * the submitter. The one piece of state the two share that is not a ring. */
static atomic_bool finished;
static atomic_bool modem_failed;

/* The submitter's own tallies. Touched only by the submitter. */
static size_t sent;
static size_t received;
static bool corrupt;

static uint8_t want_byte(size_t i)
{
	return (uint8_t)(i * 31u + 7u);
}

/*
 * The modem thread. It owns the runtime completely: it steps both stations and
 * touches nothing the submitter touches except through the rings.
 */
static void *modem(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&finished, memory_order_acquire)) {
		app_loopback_step(lb);
		if (app_loopback_elapsed(lb) > DEADLINE_SAMPLES) {
			atomic_store_explicit(&modem_failed, true,
					      memory_order_release);
			break;
		}
	}
	return NULL;
}

/* Drain b's events, collecting payload and checking it as it arrives. */
static void drain_peer(void)
{
	app_event ev;
	while (app_events_pop(b, &ev)) {
		if (ev.kind != APP_EV_RX_DATA)
			continue;
		for (size_t i = 0; i < ev.data_len; i++)
			if (ev.data[i] != want_byte(received + i))
				corrupt = true;
		received += ev.data_len;
	}
}

/* Drain a's events, watching for the connect and disconnect notifications. */
static bool drain_local(const char *needle)
{
	app_event ev;
	bool hit = false;
	while (app_events_pop(a, &ev))
		if ((ev.kind == APP_EV_HOST_MSG || ev.kind == APP_EV_REPLY) &&
		    strstr(ev.text, needle))
			hit = true;
	return hit;
}

/* Everything the submitter does every time round: poll, drain both queues on
 * both sides, and take a snapshot. This is the shape of a user interface's frame
 * handler, run flat out. */
static bool churn(void)
{
	static ardop_tlm_decoded rec;

	drain_peer();

	while (app_display_pop(a, &rec))
		;
	while (app_display_pop(b, &rec))
		;

	app_status st;
	app_snapshot(a, &st);
	app_snapshot(b, &st);
	(void)app_tx_credit(a);
	(void)app_events_pending(a);

	if (st.event_lost || atomic_load_explicit(&modem_failed,
						  memory_order_acquire))
		return false;
	return true;
}

int main(void)
{
	lb = app_loopback_open(NULL);
	if (!lb) {
		fprintf(stderr, "stress_spine: cannot build the loopback\n");
		return 1;
	}
	a = app_loopback_side(lb, 0);
	b = app_loopback_side(lb, 1);

	/* Configured before the modem thread starts, so the setup itself is not
	 * what is under test. */
	(void)app_submit_config(a, APP_CFG_MYCALL, (app_cfg_value){.str = "N0AAA"});
	(void)app_submit_config(a, APP_CFG_ARQBW,
				(app_cfg_value){.str = "2000MAX"});
	(void)app_submit_config(b, APP_CFG_MYCALL, (app_cfg_value){.str = "N0BBB"});
	(void)app_submit_config(b, APP_CFG_ARQBW,
				(app_cfg_value){.str = "2000MAX"});
	(void)app_submit_config(b, APP_CFG_LISTEN, (app_cfg_value){.flag = true});

	pthread_t th;
	if (pthread_create(&th, NULL, modem, NULL) != 0) {
		fprintf(stderr, "stress_spine: pthread_create failed\n");
		return 1;
	}

	int rc = 1;

	(void)app_submit_line(a, "ARQCALL N0BBB 2000");
	while (!drain_local("CONNECTED"))
		if (!churn())
			goto done;

	/* The transfer, offered under whatever credit is available at the
	 * instant of each call -- which is the contention this test exists to
	 * create, since the credit is republished by the other thread on every
	 * one of its steps. */
	while (sent < PAYLOAD) {
		uint8_t chunk[1024];
		size_t want = PAYLOAD - sent;
		if (want > sizeof chunk)
			want = sizeof chunk;
		for (size_t i = 0; i < want; i++)
			chunk[i] = want_byte(sent + i);

		sent += app_tx_submit(a, chunk, want, NULL);
		if (!churn())
			goto done;
	}

	while (received < PAYLOAD)
		if (!churn())
			goto done;

	(void)app_submit_line(a, "DISCONNECT");
	while (!drain_local("DISCONNECTED"))
		if (!churn())
			goto done;

	rc = corrupt ? 1 : 0;

done:
	atomic_store_explicit(&finished, true, memory_order_release);
	pthread_join(th, NULL);

	if (atomic_load_explicit(&modem_failed, memory_order_acquire)) {
		fprintf(stderr, "stress_spine: the session did not complete in "
				"%u seconds of simulated air\n",
			DEADLINE_SAMPLES / SAMPLE_RATE);
		rc = 1;
	}
	if (corrupt)
		fprintf(stderr, "stress_spine: payload corrupted\n");
	if (received != PAYLOAD && rc == 0) {
		fprintf(stderr, "stress_spine: received %zu of %u bytes\n",
			received, PAYLOAD);
		rc = 1;
	}

	app_loopback_close(lb);

	if (rc == 0)
		printf("stress_spine: %u bytes through a live session, "
		       "submitted and drained from a second thread\n",
		       PAYLOAD);
	return rc;
}
