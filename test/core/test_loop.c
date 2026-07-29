#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/loop.h"
#include "shell/platform.h"
#include "shell/runtime.h"
#include "codec/frame.h"
#include "codec/stationid.h"

/*
 * The driver loop under test, exercised through the platform seam rather than
 * the runtime directly. Two stations each run an ardop_loop over a fake
 * in-memory platform; one station's write_audio feeds the other's read_audio
 * over a shared half-duplex "air" FIFO, host commands arrive through poll_host,
 * and PTT is routed the way a real backend wires it (rt->on_ptt -> set_ptt).
 * A full ARQ session running over that proves the loop composes platform +
 * runtime correctly -- the same proof test_runtime gives for the runtime, one
 * layer out.
 */

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

/* A one-way stretch of audio: a growing FIFO of samples, drained in order. */
struct air {
	int16_t s[1000000];
	size_t len;   /* samples written. */
	size_t rd;    /* samples read. */
};

static void air_write(struct air *a, const int16_t *b, size_t n)
{
	if (a->len + n > (sizeof(a->s) / sizeof(a->s[0])))
		n = (sizeof(a->s) / sizeof(a->s[0])) - a->len;
	memcpy(a->s + a->len, b, n * sizeof(*b));
	a->len += n;
}

static size_t air_read(struct air *a, int16_t *b, size_t max)
{
	size_t avail = a->len - a->rd;
	size_t n = avail < max ? avail : max;
	memcpy(b, a->s + a->rd, n * sizeof(*b));
	a->rd += n;
	return n;
}

/* One station: runtime + loop + fake platform + captured outputs. */
struct station {
	ardop_runtime rt;
	ardop_loop loop;
	ardop_platform_ops plat;
	struct air *tx_air;   /* write_audio deposits here (outbound). */
	struct air *rx_air;   /* read_audio draws from here (inbound). */

	ardop_host_cmd q[8];  /* pending host commands for poll_host. */
	int qn, qi;

	char host[128];       /* last NOTIFY_HOST. */
	uint8_t data[1024];   /* last DELIVER_DATA. */
	size_t data_len;
	bool ptt;
	int ptt_edges;
};

/* --- fake platform ops (all take the station as ctx) --------------------- */

static size_t plat_read_audio(void *ctx, int16_t *buf, size_t max)
{
	struct station *s = ctx;
	size_t n = air_read(s->rx_air, buf, max);
	if (n < max)
		memset(buf + n, 0, (max - n) * sizeof(*buf));   /* pad silence. */
	return max;   /* always a full block: the clock ticks uniformly. */
}
static void plat_write_audio(void *ctx, const int16_t *buf, size_t n)
{
	air_write(((struct station *)ctx)->tx_air, buf, n);
}
static void plat_set_ptt(void *ctx, bool key)
{
	struct station *s = ctx;
	s->ptt = key;
	s->ptt_edges++;
}
static bool plat_poll_host(void *ctx, ardop_host_cmd *out)
{
	struct station *s = ctx;
	if (s->qi >= s->qn)
		return false;
	*out = s->q[s->qi++];
	return true;
}

/* --- runtime observer (takes the station as ctx) ------------------------- */

/* One observer: captures host/data and routes PTT to the platform the way a
 * real backend wires it (observation -> device). */
static void observe(void *ctx, const ardop_obs *o)
{
	struct station *s = ctx;
	switch (o->kind) {
	case ARDOP_OBS_HOST_MSG:
		snprintf(s->host, sizeof(s->host), "%s", o->text);
		break;
	case ARDOP_OBS_RX_DATA: {
		size_t n = o->data_len;
		if (n > sizeof(s->data))
			n = sizeof(s->data);
		memcpy(s->data, o->data, n);
		s->data_len = n;
		break;
	}
	case ARDOP_OBS_PTT:
		s->plat.set_ptt(s, o->key);
		break;
	default:
		break;
	}
}

static void station_init(struct station *s, const char *call, bool listening,
			 struct air *tx_air, struct air *rx_air)
{
	memset(s, 0, sizeof(*s));
	assert_true(ardop_runtime_init(&s->rt, kRSLens, NUM_RSLENS));
	assert_int_equal(ardop_stationid_from_str(call, &s->rt.link.mycall),
			 ARDOP_STATIONID_OK);
	s->rt.link.bw_setting = ARDOP_ARQ_BW_2000_MAX;
	s->rt.link.listening = listening;
	ardop_runtime_observe(&s->rt, observe, s);

	s->tx_air = tx_air;
	s->rx_air = rx_air;
	s->plat.ctx = s;
	s->plat.read_audio = plat_read_audio;
	s->plat.write_audio = plat_write_audio;
	s->plat.set_ptt = plat_set_ptt;
	s->plat.poll_host = plat_poll_host;
	ardop_loop_init(&s->loop, &s->rt, &s->plat);
}

/* Carry @p tx's whole transmission into the shared air, then feed it to @p rx
 * until rx has consumed it or rx keys its own reply (handing the air back). */
static void carry(struct station *tx, struct station *rx)
{
	while (ardop_runtime_tx_active(&tx->rt))
		ardop_loop_step(&tx->loop);
	while (rx->rx_air->rd < rx->rx_air->len
	       && !ardop_runtime_tx_active(&rx->rt))
		ardop_loop_step(&rx->loop);
	for (int i = 0; i < 6 && !ardop_runtime_tx_active(&rx->rt); i++)
		ardop_loop_step(&rx->loop);
}

/* Run to quiescence: let an idle station with pending host commands act, then
 * whoever is transmitting carries to the other, alternating. */
static void run(struct station *a, struct station *b)
{
	for (int r = 0; r < 40; r++) {
		if (!ardop_runtime_tx_active(&a->rt) && a->qi < a->qn)
			ardop_loop_step(&a->loop);
		if (!ardop_runtime_tx_active(&b->rt) && b->qi < b->qn)
			ardop_loop_step(&b->loop);

		if (ardop_runtime_tx_active(&a->rt))
			carry(a, b);
		else if (ardop_runtime_tx_active(&b->rt))
			carry(b, a);
		else
			break;
	}
}

static ardop_host_cmd cmd_connect(const char *target)
{
	ardop_host_cmd c = {0};
	c.kind = ARDOP_CMD_CONNECT;
	assert_int_equal(ardop_stationid_from_str(target, &c.target),
			 ARDOP_STATIONID_OK);
	c.bandwidth = ARDOP_ARQ_BW_UNDEFINED;
	return c;
}

/*
 * A full ARQ session driven through the loop + platform seam: A queues data and
 * connects to B (host commands via poll_host), the handshake and data transfer
 * run over the shared air, B's host sees CONNECTED and the payload, then A
 * disconnects. The loop -- not the test -- moves every sample and services every
 * timer.
 */
static void test_loop_session(void **state)
{
	(void)state;

	static struct air ab, ba;   /* A->B and B->A. Large; keep off the stack. */
	static struct station a, b;
	memset(&ab, 0, sizeof(ab));
	memset(&ba, 0, sizeof(ba));
	station_init(&a, "N0AAA", false, &ab, &ba);
	station_init(&b, "N0BBB", true, &ba, &ab);

	/* A's host queues data then a connect request. */
	static const uint8_t payload[] = "loop end to end";
	int plen = (int)sizeof(payload) - 1;
	a.q[a.qn].kind = ARDOP_CMD_SEND_DATA;
	a.q[a.qn].data = payload;
	a.q[a.qn].data_len = (size_t)plen;
	a.qn++;
	a.q[a.qn++] = cmd_connect("N0BBB");

	run(&a, &b);

	/* B connected and received the payload; A settled to a connected idle. */
	assert_string_equal(b.host, "CONNECTED N0AAA 2000");
	assert_int_equal(b.data_len, (size_t)plen);
	assert_memory_equal(b.data, payload, (size_t)plen);
	assert_int_equal(b.rt.link.state, ARDOP_LINK_IRS_DATA);
	assert_int_equal(a.rt.link.state, ARDOP_LINK_IDLE);
	assert_false(a.ptt);   /* PTT dropped after the last frame. */

	/* A disconnects; B tears down. */
	a.q[a.qn++].kind = ARDOP_CMD_DISCONNECT;
	run(&a, &b);

	assert_string_equal(b.host, "DISCONNECTED");
	assert_int_equal(a.rt.link.state, ARDOP_LINK_DISC);
	assert_int_equal(b.rt.link.state, ARDOP_LINK_DISC);
	assert_false(a.ptt);
	assert_false(b.ptt);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_loop_session),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
