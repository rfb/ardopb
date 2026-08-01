#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "setup.h"

#include "shell/runtime.h"
#include "codec/frame.h"
#include "codec/stationid.h"

/*
 * The main-loop body under test. Two ardop_runtime stations are wired through a
 * sample channel -- one's transmit samples become the other's captured samples --
 * and driven only through the runtime's public API (host commands in, samples in
 * and out, callbacks for host output). If a full ARQ session runs over that, the
 * runtime correctly composes demod + link + mod the way a platform backend will.
 */

static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof kRSLens / sizeof kRSLens[0]))

struct capture {
	char host[128];      /* last NOTIFY_HOST message. */
	uint8_t data[1024];  /* last DELIVER_DATA payload. */
	size_t data_len;
	bool ptt;
	int ptt_edges;       /* count of key/unkey transitions. */
	uint8_t acc[4096];   /* all DELIVER_DATA payloads concatenated. */
	size_t acc_len;
	int frames;          /* number of DELIVER_DATA callbacks. */
	bool busy;           /* last ARDOP_OBS_BUSY state. */
	int busy_changes;    /* number of busy transitions. */
};

/* One observer, capturing the host/data/ptt observations the old three
 * callbacks did. */
static void observe(void *ctx, const ardop_obs *o)
{
	struct capture *c = ctx;
	switch (o->kind) {
	case ARDOP_OBS_HOST_MSG:
		snprintf(c->host, sizeof(c->host), "%s", o->text);
		break;
	case ARDOP_OBS_RX_DATA: {
		size_t n = o->data_len;
		if (n > sizeof(c->data))
			n = sizeof(c->data);
		memcpy(c->data, o->data, n);
		c->data_len = n;
		c->frames++;
		if (c->acc_len + n <= sizeof(c->acc)) {
			memcpy(c->acc + c->acc_len, o->data, n);
			c->acc_len += n;
		}
		break;
	}
	case ARDOP_OBS_PTT:
		c->ptt = o->key;
		c->ptt_edges++;
		break;
	case ARDOP_OBS_BUSY:
		c->busy = o->busy;
		c->busy_changes++;
		break;
	default:
		break;
	}
}

static uint64_t g_now = 1000000;

static void setup(ardop_runtime *rt, struct capture *cap, const char *call,
		  bool listening)
{
	assert_true(ardop_runtime_init(rt, kRSLens, NUM_RSLENS));
	assert_int_equal(ardop_stationid_from_str(call, &rt->link.mycall),
			 ARDOP_STATIONID_OK);
	rt->link.bw_setting = ARDOP_ARQ_BW_2000_MAX;
	rt->link.listening = listening;
	memset(cap, 0, sizeof(*cap));
	ardop_runtime_observe(rt, observe, cap);
}

/* Carry @p tx's in-progress transmission to @p rx: drain transmit samples and
 * feed them in, then a little trailing silence to flush the receiver. */
static void carry(ardop_runtime *tx, ardop_runtime *rx)
{
	int16_t buf[1200];
	while (ardop_runtime_tx_active(tx)) {
		size_t n = ardop_runtime_pull_tx(tx, buf, 1200);
		if (n == 0)
			break;
		ardop_runtime_rx(rx, buf, n, g_now);
		g_now += n;
	}
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 5; i++) {
		ardop_runtime_rx(rx, buf, 1200, g_now);
		g_now += 1200;
	}
}

/* Run the exchange to quiescence: whoever is transmitting carries to the other,
 * alternating, until neither has anything to send. */
static void run(ardop_runtime *a, ardop_runtime *b)
{
	for (int i = 0; i < 24; i++) {
		if (ardop_runtime_tx_active(a))
			carry(a, b);
		else if (ardop_runtime_tx_active(b))
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
 * A full ARQ session driven entirely through the runtime API: A queues data and
 * connects to B, the handshake and data transfer run over the sample channel,
 * B's host sees CONNECTED and receives the payload, then A disconnects.
 */
static void test_runtime_session(void **state)
{
	(void)state;

	static ardop_runtime a, b;
	struct capture ca, cb;
	setup(&a, &ca, "N0AAA", false);
	setup(&b, &cb, "N0BBB", true);

	/* A queues data, then connects. */
	const uint8_t payload[] = "runtime end to end";
	int plen = (int)sizeof(payload) - 1;
	ardop_host_cmd sd = {0};
	sd.kind = ARDOP_CMD_SEND_DATA;
	sd.data = payload;
	sd.data_len = (size_t)plen;
	ardop_runtime_host(&a, &sd, g_now);

	ardop_host_cmd cc = cmd_connect("N0BBB");
	ardop_runtime_host(&a, &cc, g_now);
	assert_true(ardop_runtime_tx_active(&a));   /* the ConReq. */

	run(&a, &b);

	/* B is connected and received the payload; A is connected too. */
	assert_string_equal(cb.host, "CONNECTED N0AAA 2000");
	assert_int_equal(cb.data_len, (size_t)plen);
	assert_memory_equal(cb.data, payload, (size_t)plen);
	assert_int_equal(b.link.state, ARDOP_LINK_IRS_DATA);
	assert_int_equal(a.link.state, ARDOP_LINK_IDLE);
	assert_false(a.tx_active);   /* PTT dropped after the last frame. */
	assert_false(ca.ptt);

	/* A disconnects; B tears down. */
	ardop_host_cmd dc = {0};
	dc.kind = ARDOP_CMD_DISCONNECT;
	ardop_runtime_host(&a, &dc, g_now);
	run(&a, &b);

	assert_string_equal(cb.host, "DISCONNECTED");
	assert_int_equal(a.link.state, ARDOP_LINK_DISC);
	assert_int_equal(b.link.state, ARDOP_LINK_DISC);
}

/*
 * A FEC broadcast driven through the runtime: A queues a payload larger than one
 * frame, then FECSENDs it as a sequence of 4PSK.200.100.E (0x40) frames. There is
 * no ARQ handshake and no return path -- B is only listening, in FEC mode -- so
 * the frames flow back-to-back on a single PTT key, and B reassembles the whole
 * payload from the delivered frames.
 */
static void test_runtime_fec_broadcast(void **state)
{
	(void)state;

	static ardop_runtime a, b;
	struct capture ca, cb;
	setup(&a, &ca, "N0AAA", false);
	setup(&b, &cb, "N0BBB", true);

	/* B receives in FEC mode. */
	ardop_host_cmd bm = {0};
	bm.kind = ARDOP_CMD_SET_MODE;
	bm.arg = ARDOP_MODE_FEC;
	ardop_runtime_host(&b, &bm, g_now);

	/* A payload spanning three 0x40 frames (capacity 64 bytes each). */
	uint8_t payload[150];
	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(0x20 + (i % 90));

	ardop_host_cmd sd = {0};
	sd.kind = ARDOP_CMD_SEND_DATA;
	sd.data = payload;
	sd.data_len = sizeof(payload);
	ardop_runtime_host(&a, &sd, g_now);

	ardop_host_cmd fs = {0};
	fs.kind = ARDOP_CMD_FEC_SEND;
	fs.arg = 0x40;
	ardop_runtime_host(&a, &fs, g_now);
	assert_true(ardop_runtime_tx_active(&a));   /* first FEC frame. */

	/* Carry the whole broadcast to B; the frames run on one PTT key. */
	carry(&a, &b);

	/* B reassembled the full payload from three frames; A finished and
	 * returned to DISC (still in FEC mode), keying PTT exactly once. */
	assert_int_equal(cb.frames, 3);
	assert_int_equal(cb.acc_len, sizeof(payload));
	assert_memory_equal(cb.acc, payload, sizeof(payload));
	assert_int_equal(a.link.state, ARDOP_LINK_DISC);
	assert_int_equal(a.link.mode, ARDOP_MODE_FEC);
	assert_false(a.tx_active);
	assert_false(ca.ptt);
	assert_int_equal(ca.ptt_edges, 2);   /* one key, one unkey. */
}

/*
 * The channel-busy detector, wired through the runtime: a strong in-band tone
 * fed while idle (DISC) trips ARDOP_OBS_BUSY, and silence afterwards clears it
 * (after the detector's 5 s hold). Exercises the busy front end (window, FFT,
 * spectrum, detector) and the runtime's 1024-sample windowing.
 */
static void test_runtime_busy(void **state)
{
	(void)state;

	static ardop_runtime rt;
	struct capture cap;
	setup(&rt, &cap, "N0AAA", true);

	int16_t buf[1024];
	uint64_t t = 0;

	/* ~1.5 s of a loud 1500 Hz tone: the channel looks occupied. */
	for (int w = 0; w < 18; w++) {
		for (int i = 0; i < 1024; i++) {
			double s = 12000.0 * sin(2.0 * 3.14159265 * 1500.0
						 * (w * 1024 + i) / 12000.0);
			buf[i] = (int16_t)s;
		}
		ardop_runtime_rx(&rt, buf, 1024, t);
		t += 1024;
	}
	assert_true(cap.busy);
	assert_true(rt.busy_state);

	/* ~7 s of near-silence: past the 5 s hold, busy clears. */
	memset(buf, 0, sizeof(buf));
	for (int w = 0; w < 84; w++) {
		ardop_runtime_rx(&rt, buf, 1024, t);
		t += 1024;
	}
	assert_false(cap.busy);
	assert_true(cap.busy_changes >= 2);   /* at least one set and one clear. */
}

/* --- telemetry ------------------------------------------------------------ */

struct tlm_capture {
	int spectrum;
	int constellation;
	int status;
	int audio;
	float last_peak_bin_power;
	int peak_bin;
	uint8_t con_frame_type;
	uint8_t con_mod;
	uint16_t con_points;
	int quadrant[4];   /* constellation points per pi/2 sector. */
	int16_t last_sn;
};

static void tlm_observe(void *ctx, const ardop_telemetry *t)
{
	struct tlm_capture *c = ctx;

	switch (t->kind) {
	case ARDOP_TLM_SPECTRUM: {
		c->spectrum++;
		/* Remember the strongest bin of the loudest row seen. */
		for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++) {
			if (t->mag[i] > c->last_peak_bin_power) {
				c->last_peak_bin_power = t->mag[i];
				c->peak_bin = i;
			}
		}
		break;
	}
	case ARDOP_TLM_CONSTELLATION:
		c->constellation++;
		c->con_frame_type = t->frame_type;
		c->con_mod = t->modulation;
		c->con_points = t->n_points;
		for (uint16_t i = 0; i < t->n_points; i++) {
			/* Fold the differential phase into one of four
			 * quadrants, as the 4PSK slicer does. */
			double a = (double)t->phase_mrad[i] / 1000.0;
			while (a < 0.0)
				a += 2.0 * M_PI;
			int q = (int)(a / (M_PI / 2.0)) & 3;
			c->quadrant[q]++;
		}
		break;
	case ARDOP_TLM_STATUS:
		c->status++;
		c->last_sn = t->sn;
		break;
	case ARDOP_TLM_AUDIO:
		c->audio++;
		break;
	}
}

/*
 * Telemetry carries what a display needs, from real modulated audio.
 *
 * The constellation is the interesting assertion: the points are the
 * *differential phases the decoder slices*, so for a clean 4PSK frame they must
 * land in the four quadrants rather than scatter. If the scaling or the units
 * were wrong they would smear, and a display would show noise for a perfect
 * signal.
 */
static void test_runtime_telemetry(void **state)
{
	(void)state;

	static ardop_runtime a, b;
	struct capture ca, cb;
	static struct tlm_capture tc;

	setup(&a, &ca, "N0AAA", false);
	setup(&b, &cb, "N0BBB", true);
	memset(&tc, 0, sizeof(tc));
	ardop_runtime_set_telemetry(&b, tlm_observe, &tc);

	/* B receives in FEC mode. */
	ardop_host_cmd bm = {0};
	bm.kind = ARDOP_CMD_SET_MODE;
	bm.arg = ARDOP_MODE_FEC;
	ardop_runtime_host(&b, &bm, g_now);

	uint8_t payload[150];
	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(0x20 + (i % 90));

	ardop_host_cmd sd = {0};
	sd.kind = ARDOP_CMD_SEND_DATA;
	sd.data = payload;
	sd.data_len = sizeof(payload);
	ardop_runtime_host(&a, &sd, g_now);

	ardop_host_cmd fs = {0};
	fs.kind = ARDOP_CMD_FEC_SEND;
	fs.arg = 0x40;   /* 4PSK.200.100: four constellation clusters. */
	ardop_runtime_host(&a, &fs, g_now);
	carry(&a, &b);

	/* The payload arrived, so the audio really was decoded. */
	assert_int_equal(cb.acc_len, sizeof(payload));

	/* A spectrum row per 1024 captured samples, ungated by link state. */
	assert_true(tc.spectrum > 0);
	/* 4PSK.200.100 is a single carrier centred on 1500 Hz. The strongest
	 * bin must land there (bin 128 of the transform, 103 of the slice),
	 * which is what proves the row is a real spectrum and not zeroes. */
	int centre = 128 - ARDOP_BUSY_FIRST_BIN;
	assert_true(tc.peak_bin > centre - 12 && tc.peak_bin < centre + 12);

	/* One constellation per decoded frame (three frames of payload). */
	assert_int_equal(tc.constellation, cb.frames);
	assert_int_equal(tc.con_frame_type, 0x40);
	assert_int_equal(tc.con_mod, ARDOP_MOD_4PSK);
	assert_true(tc.con_points > 0);

	/* Every quadrant used, and none holding a wild share: a clean 4PSK
	 * frame slices into four roughly equal clusters. */
	int total = 0;
	for (int q = 0; q < 4; q++) {
		assert_true(tc.quadrant[q] > 0);
		total += tc.quadrant[q];
	}
	for (int q = 0; q < 4; q++)
		assert_true(tc.quadrant[q] < total * 3 / 4);

	/* The status mirror ran and carried this frame's S/N. */
	assert_true(tc.status > 0);
	assert_true(tc.last_sn > 0);
}

/* With no sink registered, nothing is gathered and -- the property that
 * matters -- the busy detector behaves exactly as it did before telemetry
 * existed, because it keeps its own accumulator. */
static void test_runtime_telemetry_off_by_default(void **state)
{
	(void)state;

	static ardop_runtime a, b;
	struct capture ca, cb;
	static struct tlm_capture tc;

	setup(&a, &ca, "N0AAA", false);
	setup(&b, &cb, "N0BBB", true);
	memset(&tc, 0, sizeof(tc));
	/* Deliberately no ardop_runtime_set_telemetry. */

	ardop_host_cmd bm = {0};
	bm.kind = ARDOP_CMD_SET_MODE;
	bm.arg = ARDOP_MODE_FEC;
	ardop_runtime_host(&b, &bm, g_now);

	uint8_t payload[64];
	memset(payload, 'x', sizeof(payload));
	ardop_host_cmd sd = {0};
	sd.kind = ARDOP_CMD_SEND_DATA;
	sd.data = payload;
	sd.data_len = sizeof(payload);
	ardop_runtime_host(&a, &sd, g_now);

	ardop_host_cmd fs = {0};
	fs.kind = ARDOP_CMD_FEC_SEND;
	fs.arg = 0x40;
	ardop_runtime_host(&a, &fs, g_now);
	carry(&a, &b);

	assert_int_equal(cb.acc_len, sizeof(payload));
	assert_int_equal(tc.spectrum, 0);
	assert_int_equal(tc.constellation, 0);
	assert_int_equal(tc.status, 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_runtime_session),
		cmocka_unit_test(test_runtime_fec_broadcast),
		cmocka_unit_test(test_runtime_busy),
		cmocka_unit_test(test_runtime_telemetry),
		cmocka_unit_test(test_runtime_telemetry_off_by_default),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
