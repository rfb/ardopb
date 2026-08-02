#include "app/spine.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/ring.h"
#include "shell/host.h"
#include "shell/loop.h"

/**
 * @file spine.c
 * @brief The embedding seam (see spine.h).
 *
 * Everything in this file runs on one of two threads and it is always obvious
 * which: functions taking the modem thread are grouped under "modem thread"
 * banners, the submitting side under "submitter". The rings between them are the
 * only shared mutable state, plus four atomics.
 *
 * The single most important thing here is the transmit credit, so it is worth
 * stating why it is not simply "read `rt->link.tx_len`". That field belongs to
 * the runtime, and the rule is that nothing off the modem thread touches the
 * runtime for any reason. So the modem thread *publishes* a credit -- a
 * monotonic byte limit -- and the submitter compares its own monotonic total
 * against it. This is `shell/ring.c`'s counter idiom applied to bytes of
 * payload rather than samples, and it means the submitter needs no lock and no
 * access to the link at all.
 */

/* The RS parity lengths the frame set uses. Same table as shell/main.c:45; it
 * is a property of the waveform, not of the program. */
static const int kRSLens[] = {2, 4, 8, 16, 32, 36, 50, 64};
#define NUM_RSLENS ((int)(sizeof(kRSLens) / sizeof(kRSLens[0])))

/* --- the downward record layout ------------------------------------------- */

typedef enum {
	CMD_SEND_DATA = 1,
	CMD_HOST_CMD,
	CMD_HOST_LINE,
	CMD_CONFIG,
} cmd_kind;

/* Fixed part of a downward record; a variable tail follows for the kinds that
 * need one (payload bytes, a command line, a config string). */
typedef struct {
	uint8_t kind;        /* cmd_kind */
	uint8_t cfg_key;     /* app_cfg_key, for CMD_CONFIG */
	uint32_t tail;       /* bytes following this header */
	long num;            /* CMD_CONFIG numeric/boolean value */
	ardop_host_cmd cmd;  /* CMD_HOST_CMD; its `data` pointer is not used */
} cmd_hdr;

/* Largest downward record: a header plus a full transmit chunk, and the longest
 * command line ardop_host_command will look at. */
#define CMD_TAIL_MAX 3000u
#define CMD_RECORD_MAX (sizeof(cmd_hdr) + (APP_TX_CHUNK > CMD_TAIL_MAX \
					   ? APP_TX_CHUNK : CMD_TAIL_MAX))

/* --- the upward event record layout --------------------------------------- */

typedef struct {
	uint8_t kind;      /* app_event_kind */
	uint8_t flag;
	char tag[4];
	int32_t code;      /* app_fault on FAULT, app_device_code on DEVICE */
	uint32_t text_len; /* bytes of text, NUL excluded */
	uint32_t data_len;
} ev_hdr;

#define EV_RECORD_MAX (sizeof(ev_hdr) + APP_TEXT_MAX + ARDOP_DEMOD_MAX_PAYLOAD)

/* --- the spine ------------------------------------------------------------- */

struct app_spine {
	ardop_runtime rt;
	ardop_loop loop;

	const ardop_platform_ops *plat;   /* modem thread */
	const app_tnc_ops *tnc;           /* modem thread */

	/* Rings, and the storage they were handed. */
	app_ring cmdq, displayq, eventq;
	uint8_t *cmd_buf, *display_buf, *event_buf;
	size_t event_high, event_low;

	/* Transmit credit. See the file comment.
	 *
	 * tx_offered is owned by the submitter and tx_applied by the modem
	 * thread, and neither is atomic *on purpose*: exactly one thread may
	 * touch each, so making them atomic would paper over a violation of that
	 * contract instead of letting ThreadSanitizer report it. */
	uint64_t tx_offered;
	uint64_t tx_applied;
	_Atomic uint64_t tx_limit;
	_Atomic int tx_reason;

	/* Ownership and shutdown. */
	_Atomic bool tnc_attached;
	bool tnc_attached_local;   /* modem thread's edge detector */

	/* Whether transmission is permitted, and why not. Both modem-owned;
	 * app_snapshot publishes copies. Recoverable, unlike the one-way flag
	 * this replaced -- see app_run_state in spine.h. */
	app_run_state run;
	app_fault fault;

	/* Counters, published for app_snapshot. */
	_Atomic uint_least64_t display_drops;
	_Atomic uint_least64_t event_lost;
	bool events_congested;
	bool overflow_reported;

	/* Scratch. Each belongs to exactly one thread; they live here rather than
	 * on a stack because they are large and the audio path must not allocate. */
	uint8_t enc[ARDOP_TLM_MAX_RECORD];   /* modem thread */
	uint8_t cmd_rec[CMD_RECORD_MAX];     /* modem thread */
	uint8_t ev_rec[EV_RECORD_MAX];       /* submitter */
	uint8_t disp_rec[ARDOP_TLM_MAX_RECORD]; /* submitter */
};

const char *app_tx_status_str(app_tx_status s)
{
	switch (s) {
	case APP_TX_OK:        return "ok";
	case APP_TX_FULL:      return "link queue full";
	case APP_TX_TNC_OWNS:  return "TNC client owns the link";
	case APP_TX_CONGESTED: return "event queue congested";
	case APP_TX_NO_ROOM:   return "command queue full";
	case APP_TX_FAULTED:   return "device fault; reselect or retry";
	case APP_TX_NO_DEVICE: return "no audio device";
	case APP_TX_CLOSED:    return "stopping";
	}
	return "unknown";
}

/* --- modem thread: emitting upward ---------------------------------------- */

/* Write header + text + data as one record.
 *
 * app_ring_write takes two parts and an event has up to three, so the two
 * variable parts are staged into one buffer first. That costs a memcpy of at
 * most 1 kB on the modem thread and buys an all-or-nothing record; the
 * alternative is a three-part ring write that nothing else would use.
 *
 * The reserve is what makes an overflow reportable: ordinary records must leave
 * APP_EVENT_RESERVE bytes free, so when one is finally refused there is still
 * room for the single record that says so. */
static bool queue_event(app_spine *sp, app_event_kind kind, int code, bool flag,
			const char *tag, const char *text, const uint8_t *data,
			size_t data_len, size_t reserve)
{
	ev_hdr h;
	memset(&h, 0, sizeof h);
	h.kind = (uint8_t)kind;
	h.code = (int32_t)code;
	h.flag = flag;
	if (tag) {
		snprintf(h.tag, sizeof h.tag, "%s", tag);
	}

	size_t text_len = 0;
	if (text) {
		text_len = strlen(text);
		if (text_len > APP_TEXT_MAX - 1)
			text_len = APP_TEXT_MAX - 1;
	}
	if (data_len > ARDOP_DEMOD_MAX_PAYLOAD)
		data_len = ARDOP_DEMOD_MAX_PAYLOAD;

	h.text_len = (uint32_t)text_len;
	h.data_len = (uint32_t)data_len;

	uint8_t tail[APP_TEXT_MAX + ARDOP_DEMOD_MAX_PAYLOAD];
	if (text_len)
		memcpy(tail, text, text_len);
	if (data_len)
		memcpy(tail + text_len, data, data_len);

	return app_ring_write(&sp->eventq, &h, sizeof h, tail,
			      text_len + data_len, reserve);
}

/* Queue an event, and account for it if the queue would not take it.
 *
 * Losslessness holds up to the moment this returns false, and the design keeps
 * that moment out of reach: the credit is cut at half full, so an application
 * that drains at all never gets here. If it does happen the count is published
 * and one fault record goes through the reserve -- what is never done is losing
 * a record silently. */
static void emit(app_spine *sp, app_event_kind kind, int code, bool flag,
		 const char *tag, const char *text, const uint8_t *data,
		 size_t data_len)
{
	if (queue_event(sp, kind, code, flag, tag, text, data, data_len,
			APP_EVENT_RESERVE))
		return;

	atomic_fetch_add_explicit(&sp->event_lost, 1, memory_order_relaxed);
	if (!sp->overflow_reported) {
		sp->overflow_reported = true;
		(void)queue_event(sp, APP_EV_FAULT, APP_FAULT_QUEUE, false, NULL,
				  "event queue overflowed; received data was lost",
				  NULL, 0, 0);
	}
}

/* Queue one encoded telemetry record. Lossy by design: a refusal is counted and
 * forgotten.
 *
 * Note this drops the *newest* record rather than the oldest, which is the
 * opposite of shell/telemetry_tcp.c. That transport can drop the oldest because
 * it is single-threaded; here the read index belongs to the consumer alone. The
 * difference costs nothing, because a consumer drains the whole ring every frame
 * -- so the queue only ever fills within one frame interval, and the same number
 * of rows go missing either way. */
static void queue_display(app_spine *sp, const uint8_t *rec, size_t n)
{
	if (!app_ring_write(&sp->displayq, rec, n, NULL, 0, 0))
		atomic_fetch_add_explicit(&sp->display_drops, 1,
					  memory_order_relaxed);
}

/* --- modem thread: the observer and the telemetry sink -------------------- */

/*
 * The single observer. ARDOP_MAX_OBSERVERS is 4 and there is no way to
 * unregister, so the spine takes one slot and fans out on the far side of the
 * event queue, where a slow consumer costs nothing. Observers run on the audio
 * path; the right number of callbacks to make there is the smallest one.
 *
 * Every pointer in an ardop_obs is borrowed, and borrowed more briefly than it
 * looks: `text` and `data` address buffers inside the link that are overwritten
 * by the *next link step*, and one step can perform several actions. So they are
 * copied here, not referenced.
 */
static void observe(void *ctx, const ardop_obs *o)
{
	app_spine *sp = ctx;

	switch (o->kind) {
	case ARDOP_OBS_PTT:
		/* Resolved now, never captured at registration: this fires from
		 * inside ardop_runtime_pull_tx, and a device change between the
		 * two would key a backend that no longer exists. */
		if (sp->plat && sp->plat->set_ptt)
			sp->plat->set_ptt(sp->plat->ctx, o->key);
		break;

	case ARDOP_OBS_HOST_MSG:
		emit(sp, APP_EV_HOST_MSG, 0, false, NULL, o->text, NULL, 0);
		if (sp->tnc && sp->tnc->notify)
			sp->tnc->notify(sp->tnc->ctx, o->text);
		break;

	case ARDOP_OBS_RX_DATA:
		emit(sp, APP_EV_RX_DATA, 0, false, o->tag, NULL, o->data,
		     o->data_len);
		if (sp->tnc && sp->tnc->send_data)
			sp->tnc->send_data(sp->tnc->ctx, o->tag, o->data,
					   o->data_len);
		break;

	/* The rest is discrete state the runtime already mirrors into the
	 * ARDOP_TLM_STATUS record on the display queue, and putting it on the
	 * lossless queue as well would be two sources for one truth. STATE is
	 * the exception, and only for the one field STATUS does not carry. */
	case ARDOP_OBS_STATE:
		/*
		 * The remote callsign, which analysis/16 §7 calls "the first
		 * thing an operator looks for" and which nothing else carries.
		 *
		 * ARDOP_TLM_STATUS has state, mode, busy, PTT, S/N, quality,
		 * bandwidth and buffer length -- but not who you are talking to,
		 * so today it only reaches a host client as a CONNECTED line.
		 * Embedded it is free: the observation already has it, and this
		 * is the callback that was throwing it away.
		 */
		emit(sp, APP_EV_STATE, (int)o->state, false, NULL,
		     o->remote ? o->remote : "", NULL, 0);
		break;
	case ARDOP_OBS_MODE:
	case ARDOP_OBS_BANDWIDTH:
	case ARDOP_OBS_RX_FRAME:
	case ARDOP_OBS_RX_FRAME_BAD:
	case ARDOP_OBS_TX_FRAME:
	case ARDOP_OBS_BUSY:
	case ARDOP_OBS_BUFFER:
		break;
	}
}

/* The telemetry sink. Called from the audio path; encodes and queues, nothing
 * more. The wire encoding is deliberate: the bytes in the ring are exactly what
 * a modem's telemetry port emits, so a display fed from a remote station
 * substitutes behind app_display_pop with no change to the consumer. */
static void sink(void *ctx, const ardop_telemetry *t)
{
	app_spine *sp = ctx;

	size_t n = ardop_tlm_encode(t, sp->enc, sizeof sp->enc);
	if (n)
		queue_display(sp, sp->enc, n);
}

/* Forward: app_open publishes an initial credit, and publish_credit belongs
 * with the rest of the modem-thread code below. */
static void publish_credit(app_spine *sp);

/* --- lifetime -------------------------------------------------------------- */

app_spine *app_open(const app_config *cfg)
{
	app_config def;
	memset(&def, 0, sizeof def);
	if (!cfg)
		cfg = &def;

	size_t cmd_bytes = cfg->cmd_bytes ? cfg->cmd_bytes
					  : APP_CMD_BYTES_DEFAULT;
	size_t display_bytes = cfg->display_bytes ? cfg->display_bytes
						  : APP_DISPLAY_BYTES_DEFAULT;
	size_t event_bytes = cfg->event_bytes ? cfg->event_bytes
					      : APP_EVENT_BYTES_DEFAULT;

	/* A ring smaller than its largest record can never accept one, which is
	 * a configuration error rather than a runtime condition. */
	if (cmd_bytes < CMD_RECORD_MAX + APP_RING_OVERHEAD ||
	    display_bytes < ARDOP_TLM_MAX_RECORD + APP_RING_OVERHEAD ||
	    event_bytes < EV_RECORD_MAX + APP_RING_OVERHEAD + APP_EVENT_RESERVE)
		return NULL;

	app_spine *sp = calloc(1, sizeof *sp);
	if (!sp)
		return NULL;

	sp->cmd_buf = malloc(cmd_bytes);
	sp->display_buf = malloc(display_bytes);
	sp->event_buf = malloc(event_bytes);
	if (!sp->cmd_buf || !sp->display_buf || !sp->event_buf) {
		app_close(sp);
		return NULL;
	}

	if (!app_ring_init(&sp->cmdq, sp->cmd_buf, cmd_bytes) ||
	    !app_ring_init(&sp->displayq, sp->display_buf, display_bytes) ||
	    !app_ring_init(&sp->eventq, sp->event_buf, event_bytes)) {
		app_close(sp);
		return NULL;
	}

	/* Half full cuts the credit, quarter full restores it. The hysteresis
	 * keeps a consumer that drains in bursts from toggling the transmitter's
	 * admission on every step. */
	sp->event_high = event_bytes / 2;
	sp->event_low = event_bytes / 4;

	if (!ardop_runtime_init(&sp->rt, kRSLens, NUM_RSLENS)) {
		app_close(sp);
		return NULL;
	}

	ardop_runtime_observe(&sp->rt, observe, sp);
	if (cfg->telemetry)
		ardop_runtime_set_telemetry(&sp->rt, sink, sp);

	ardop_loop_init(&sp->loop, &sp->rt, NULL);

	/* Publish once here, so the credit is true from the moment the spine
	 * exists rather than from the first step. The link's queue really is
	 * empty and really can take bytes; they wait in the command ring until
	 * the modem thread runs. Leaving it at zero until then would report
	 * "queue full" about an empty queue -- a lie a settings screen would
	 * show for as long as it took the audio device to open. */
	publish_credit(sp);
	return sp;
}

void app_close(app_spine *sp)
{
	if (!sp)
		return;

	sp->run = APP_RUN_CLOSING;

	/* Unkey before letting go of the backend. The caller closes the device
	 * after this returns and the PTT after that, so the keying line outlives
	 * the device that keyed it. */
	if (sp->plat && sp->plat->set_ptt)
		sp->plat->set_ptt(sp->plat->ctx, false);

	free(sp->cmd_buf);
	free(sp->display_buf);
	free(sp->event_buf);
	free(sp);
}

bool app_set_platform(app_spine *sp, const ardop_platform_ops *ops, size_t block)
{
	/* A device swap mid-transmission leaves the radio keyed with nothing able
	 * to unkey it: PTT-down is emitted from inside ardop_runtime_pull_tx,
	 * against whatever backend is bound when the frame ends. Refuse, and let
	 * the caller disconnect first. */
	if (ardop_runtime_tx_active(&sp->rt))
		return false;

	if (sp->plat && sp->plat->set_ptt && sp->plat != ops)
		sp->plat->set_ptt(sp->plat->ctx, false);

	sp->plat = ops;

	/* ardop_loop holds the ops by pointer and reads read_audio unguarded, so
	 * it is re-initialised rather than patched. The clock is preserved: it is
	 * a monotonic sample count and nothing requires it to belong to one
	 * device.
	 *
	 * The block is NOT preserved, deliberately. It used to be, and because
	 * ardop_loop_init always leaves it non-zero the restore always fired --
	 * so a newly bound backend's smaller preferred block was overwritten by
	 * the previous device's every single time, and every swap silently
	 * reverted to the 100 ms default. It belongs to the backend, so it
	 * arrives with the backend. */
	uint64_t t = sp->loop.t;
	ardop_loop_init(&sp->loop, &sp->rt, ops);
	sp->loop.t = t;
	if (block && block <= ARDOP_LOOP_BLOCK)
		sp->loop.block = block;

	/* Binding a device makes transmission possible, so say so now rather
	 * than at the next step: a settings screen that has just opened a sound
	 * card should not show "no audio device" until the modem happens to run. */
	publish_credit(sp);
	return true;
}

void app_report_guest(app_spine *sp, app_guest_code code, const char *text)
{
	if (sp)
		emit(sp, APP_EV_GUEST, (int)code, false, NULL, text, NULL, 0);
}

void app_set_tnc(app_spine *sp, const app_tnc_ops *tnc)
{
	sp->tnc = tnc;
}

ardop_runtime *app_runtime(app_spine *sp)
{
	return &sp->rt;
}

app_run_state app_run_state_of(const app_spine *sp)
{
	return sp->run;
}

app_fault app_fault_state(const app_spine *sp)
{
	return sp->fault;
}

app_run_state app_clear_fault(app_spine *sp)
{
	app_run_state was = sp->run;

	/* Closing is terminal; a fault is not. Nothing recovers a spine that is
	 * being torn down, and pretending otherwise would let a device manager
	 * resurrect one mid-shutdown. */
	if (sp->run == APP_RUN_FAULTED) {
		sp->run = APP_RUN_OK;
		sp->fault = APP_FAULT_NONE;
		sp->overflow_reported = false;
		publish_credit(sp);
	}
	return was;
}

uint64_t app_elapsed(const app_spine *sp)
{
	return sp->loop.t;
}

/* --- modem thread: applying downward records ------------------------------ */

/* Run a TNC line and surface its reply. ardop_host_command writes "" for the
 * commands that warrant no reply, which is not an event. */
static void run_line(app_spine *sp, const char *line)
{
	char reply[APP_TEXT_MAX];
	ardop_host_command(&sp->rt, line, sp->loop.t, reply, sizeof reply);
	if (reply[0])
		emit(sp, APP_EV_REPLY, 0, false, NULL, reply, NULL, 0);
}

/* True for the commands that start, steer or end a session -- the ones that
 * would collide with a guest's. Configuration is deliberately not in this set:
 * refusing a guest's MYCALL would break Pat's startup handshake, and refusing
 * the application's would be the same mistake pointed the other way. */
static bool line_is_session(const char *up)
{
	static const char *const kSession[] = {
		"ARQCALL", "DISCONNECT", "ABORT", "DD", "FECSEND", "SENDID",
		"LISTEN", "BREAK",
	};
	for (size_t i = 0; i < sizeof kSession / sizeof kSession[0]; i++) {
		size_t n = strlen(kSession[i]);
		if (strncmp(up, kSession[i], n) == 0 &&
		    (up[n] == '\0' || up[n] == ' '))
			return true;
	}
	return false;
}

static bool cmd_is_session(ardop_host_cmd_kind k)
{
	switch (k) {
	case ARDOP_CMD_CONNECT:
	case ARDOP_CMD_DISCONNECT:
	case ARDOP_CMD_ABORT:
	case ARDOP_CMD_FEC_SEND:
	case ARDOP_CMD_SEND_ID:
	case ARDOP_CMD_BREAK:
		return true;
	case ARDOP_CMD_SEND_DATA:
	case ARDOP_CMD_SET_MODE:
		return false;
	}
	return false;
}

static void refuse(app_spine *sp)
{
	emit(sp, APP_EV_REPLY, 0, false, NULL,
	     "FAULT TNC client owns the link", NULL, 0);
}

/* Format a setting as its TNC line and run it. One validator for every setting,
 * shared with guests -- see app_cfg_key in spine.h. */
static void apply_config(app_spine *sp, const cmd_hdr *h, const char *str)
{
	/* The value is bounded by APP_CFG_STR_MAX at submission, so no keyword
	 * plus value can overflow this and nothing here can truncate. */
	char line[APP_CFG_STR_MAX + 32];

	switch ((app_cfg_key)h->cfg_key) {
	case APP_CFG_MYCALL:
		snprintf(line, sizeof line, "MYCALL %s", str);
		break;
	case APP_CFG_GRIDSQUARE:
		snprintf(line, sizeof line, "GRIDSQUARE %s", str);
		break;
	case APP_CFG_ARQBW:
		snprintf(line, sizeof line, "ARQBW %s", str);
		break;
	case APP_CFG_PROTOCOLMODE:
		snprintf(line, sizeof line, "PROTOCOLMODE %s", str);
		break;
	case APP_CFG_FECMODE:
		snprintf(line, sizeof line, "FECMODE %s", str);
		break;
	case APP_CFG_FECREPEATS:
		snprintf(line, sizeof line, "FECREPEATS %ld", h->num);
		break;
	case APP_CFG_LISTEN:
		snprintf(line, sizeof line, "LISTEN %s",
			 h->num ? "TRUE" : "FALSE");
		break;
	case APP_CFG_AUTOBREAK:
		snprintf(line, sizeof line, "AUTOBREAK %s",
			 h->num ? "TRUE" : "FALSE");
		break;
	case APP_CFG_FSKONLY:
		snprintf(line, sizeof line, "FSKONLY %s",
			 h->num ? "TRUE" : "FALSE");
		break;
	case APP_CFG_USE600MODES:
		snprintf(line, sizeof line, "USE600MODES %s",
			 h->num ? "TRUE" : "FALSE");
		break;
	case APP_CFG_ENABLEPINGACK:
		snprintf(line, sizeof line, "ENABLEPINGACK %s",
			 h->num ? "TRUE" : "FALSE");
		break;

	case APP_CFG_BUSYDET: {
		/* The one direct write. Busy sensitivity lives on the runtime,
		 * not the link, and there is no host command for it -- so there
		 * is no text path to share. If BUSYDET is ever added to
		 * shell/host.c, delete this case and let it join the rest. */
		long v = h->num;
		if (v < 0)
			v = 0;
		if (v > 10)
			v = 10;
		sp->rt.busy_det = (int)v;
		char msg[APP_TEXT_MAX];
		snprintf(msg, sizeof msg, "BUSYDET now %d", sp->rt.busy_det);
		emit(sp, APP_EV_REPLY, 0, false, NULL, msg, NULL, 0);
		return;
	}
	}

	run_line(sp, line);
}

/* Apply one downward record. */
static void apply(app_spine *sp, const uint8_t *rec, size_t n)
{
	cmd_hdr h;
	if (n < sizeof h)
		return;
	memcpy(&h, rec, sizeof h);

	const char *tail = (const char *)rec + sizeof h;
	size_t tail_len = n - sizeof h;
	bool owned = sp->tnc_attached_local;

	switch ((cmd_kind)h.kind) {
	case CMD_SEND_DATA: {
		/* Credit is cut to zero while a guest is attached, so this
		 * cannot arrive then -- but the check is free and the invariant
		 * is the one that keeps two byte streams from interleaving. */
		if (owned || sp->run != APP_RUN_OK)
			return;

		ardop_host_cmd c;
		memset(&c, 0, sizeof c);
		c.kind = ARDOP_CMD_SEND_DATA;
		c.data = (const uint8_t *)tail;
		c.data_len = tail_len;

		size_t before = sp->rt.link.tx_len;
		ardop_runtime_host(&sp->rt, &c, sp->loop.t);
		size_t took = sp->rt.link.tx_len - before;

		/* The link truncates silently past its capacity and reports
		 * nothing (core/link/link.c:1023-1039). The credit makes that
		 * unreachable; assert it anyway, because "unreachable by
		 * construction" is a claim with a short half-life. */
		if (took != tail_len) {
			char msg[APP_TEXT_MAX];
			snprintf(msg, sizeof msg,
				 "FAULT transmit queue took %zu of %zu bytes",
				 took, tail_len);
			emit(sp, APP_EV_FAULT, APP_FAULT_OTHER, false, NULL, msg,
			     NULL, 0);
		}
		sp->tx_applied += tail_len;
		break;
	}

	case CMD_HOST_CMD:
		if (owned && cmd_is_session(h.cmd.kind)) {
			refuse(sp);
			return;
		}
		if (sp->run != APP_RUN_OK)
			return;
		ardop_runtime_host(&sp->rt, &h.cmd, sp->loop.t);
		break;

	case CMD_HOST_LINE: {
		char line[CMD_TAIL_MAX];
		size_t len = tail_len < sizeof line - 1 ? tail_len
							: sizeof line - 1;
		memcpy(line, tail, len);
		line[len] = '\0';

		char up[CMD_TAIL_MAX];
		size_t i = 0;
		for (; line[i]; i++)
			up[i] = (char)((line[i] >= 'a' && line[i] <= 'z')
					       ? line[i] - ('a' - 'A')
					       : line[i]);
		up[i] = '\0';

		if (owned && line_is_session(up)) {
			refuse(sp);
			return;
		}
		if (sp->run != APP_RUN_OK)
			return;
		run_line(sp, line);
		break;
	}

	case CMD_CONFIG: {
		char str[APP_CFG_STR_MAX];
		size_t len = tail_len < sizeof str - 1 ? tail_len
						      : sizeof str - 1;
		memcpy(str, tail, len);
		str[len] = '\0';
		if (sp->run != APP_RUN_OK)
			return;
		/* Configuration is applied whoever is attached, deliberately.
		 * See line_is_session(). */
		apply_config(sp, &h, str);
		break;
	}
	}
}

/* --- modem thread: the step ------------------------------------------------ */

/* Publish the credit the submitter compares against.
 *
 *   limit  = bytes the link could have taken in total by now
 *   offered = bytes the submitter has handed over in total
 *
 * so limit - offered is the room the link has, minus what is already in the
 * command ring and not yet applied. No double counting, and limit only ever
 * grows, because tx_applied grows and the link's own queue only drains. */
static void publish_credit(app_spine *sp)
{
	size_t room = sp->rt.link.tx_cap - sp->rt.link.tx_len;
	uint64_t lim = sp->tx_applied + room;
	app_tx_status why = room ? APP_TX_OK : APP_TX_FULL;

	/* With no backend bound the loop never runs, so anything accepted here
	 * would sit in the link's queue forever. Reporting credit in that state
	 * invites a caller to submit into a black hole until the 16 kB fills and
	 * then wonder why nothing transmitted. */
	if (!sp->plat) {
		lim = sp->tx_applied;
		why = APP_TX_NO_DEVICE;
	}
	if (sp->events_congested) {
		lim = sp->tx_applied;
		why = APP_TX_CONGESTED;
	}
	if (sp->tnc_attached_local) {
		lim = sp->tx_applied;
		why = APP_TX_TNC_OWNS;
	}
	if (sp->run == APP_RUN_FAULTED) {
		lim = sp->tx_applied;
		why = APP_TX_FAULTED;
	}
	if (sp->run == APP_RUN_CLOSING) {
		lim = sp->tx_applied;
		why = APP_TX_CLOSED;
	}

	atomic_store_explicit(&sp->tx_reason, (int)why, memory_order_relaxed);
	atomic_store_explicit(&sp->tx_limit, lim, memory_order_release);
}

void app_step(app_spine *sp)
{
	/* Drain everything queued, not one record: a burst of settings applied
	 * one per 100 ms audio block would take a visible moment to land. */
	for (;;) {
		size_t n = app_ring_read(&sp->cmdq, sp->cmd_rec,
					 sizeof sp->cmd_rec);
		if (!n)
			break;
		apply(sp, sp->cmd_rec, n);
	}

	if (sp->tnc) {
		sp->tnc->service(sp->tnc->ctx, &sp->rt, sp->loop.t);

		bool now = sp->tnc->attached(sp->tnc->ctx);
		if (now != sp->tnc_attached_local) {
			sp->tnc_attached_local = now;
			atomic_store_explicit(&sp->tnc_attached, now,
					      memory_order_release);
			emit(sp, APP_EV_OWNER, 0, now, NULL,
			     now ? "TNC client attached; it owns the link"
				 : "TNC client left",
			     NULL, 0);
		}
	}

	if (sp->plat)
		ardop_loop_step(&sp->loop);

	/* The path from the event queue's depth back to transmit admission.
	 * Cutting the credit stalls an outgoing transfer gracefully -- the link
	 * drains, the peer idles, nothing is lost -- which is what makes a slow
	 * consumer safe. It cannot help against a consumer that has stopped
	 * draining entirely, because inbound payload arrives when the peer
	 * transmits and no local decision stops that; the reserve and the
	 * event_lost count exist for that case. */
	size_t used = app_ring_used(&sp->eventq);
	if (used > sp->event_high)
		sp->events_congested = true;
	else if (used < sp->event_low)
		sp->events_congested = false;

	publish_credit(sp);
}

void app_report_device(app_spine *sp, app_device_code code, const char *text)
{
	emit(sp, APP_EV_DEVICE, (int)code, false, NULL, text ? text : "", NULL, 0);
}

void app_report_fault(app_spine *sp, app_fault code, const char *what)
{
	/* First fault wins, matching the backend's own latch. A capture loss that
	 * also drops the keying line should read as one failure, not two. */
	if (sp->run != APP_RUN_OK)
		return;

	char msg[APP_TEXT_MAX];
	bool was_connected = sp->rt.link.state != ARDOP_LINK_DISC;

	/* Say what happened to the session in the same breath. An operator who
	 * reads "capture device lost" and nothing else will wonder whether their
	 * transfer is still running; it is not, and it will not be resumed. */
	snprintf(msg, sizeof msg, "FAULT %s%s", what ? what : "device lost",
		 was_connected ? " -- the session was disconnected" : "");

	if (was_connected) {
		ardop_host_cmd c;
		memset(&c, 0, sizeof c);
		c.kind = ARDOP_CMD_DISCONNECT;
		ardop_runtime_host(&sp->rt, &c, sp->loop.t);
	}

	/* Force the line down whatever the link thinks: the point of a fault is
	 * that the modem's model of the device is no longer true. */
	if (sp->plat && sp->plat->set_ptt)
		sp->plat->set_ptt(sp->plat->ctx, false);

	if (sp->tnc && sp->tnc->notify)
		sp->tnc->notify(sp->tnc->ctx, msg);

	emit(sp, APP_EV_FAULT, (int)code, false, NULL, msg, NULL, 0);
	sp->run = APP_RUN_FAULTED;
	sp->fault = code;
	publish_credit(sp);
}

/* --- submitter: downward --------------------------------------------------- */

static bool submit(app_spine *sp, const cmd_hdr *h, const void *tail,
		   size_t tail_len)
{
	return app_ring_write(&sp->cmdq, h, sizeof *h, tail, tail_len, 0);
}

/* Why the credit is exhausted.
 *
 * The published reason describes the link as it stood at the last app_step. A
 * submitter can run out of credit before then, by offering the link's whole
 * capacity in one burst -- and reporting the stale "ok" in that case would be a
 * zero return with no explanation. The submitter's own in-flight bytes are what
 * filled the queue, so the honest answer is the same one the modem thread would
 * publish on its next step. */
static app_tx_status tx_reason(app_spine *sp)
{
	app_tx_status r = (app_tx_status)atomic_load_explicit(
		&sp->tx_reason, memory_order_relaxed);
	return r == APP_TX_OK ? APP_TX_FULL : r;
}

size_t app_tx_credit(app_spine *sp)
{
	uint64_t limit = atomic_load_explicit(&sp->tx_limit,
					      memory_order_acquire);
	if (limit <= sp->tx_offered)
		return 0;

	uint64_t credit = limit - sp->tx_offered;
	if (credit > APP_TX_CHUNK)
		credit = APP_TX_CHUNK;
	return (size_t)credit;
}

size_t app_tx_submit(app_spine *sp, const void *data, size_t len,
		     app_tx_status *why)
{
	size_t take = app_tx_credit(sp);
	if (take > len)
		take = len;

	if (take == 0) {
		if (why)
			*why = tx_reason(sp);
		return 0;
	}

	cmd_hdr h;
	memset(&h, 0, sizeof h);
	h.kind = CMD_SEND_DATA;
	h.tail = (uint32_t)take;

	if (!submit(sp, &h, data, take)) {
		if (why)
			*why = APP_TX_NO_ROOM;
		return 0;
	}

	/* Only after the ring's own release store has published the bytes. */
	sp->tx_offered += take;

	if (why)
		*why = (take == len) ? APP_TX_OK : tx_reason(sp);
	return take;
}

bool app_submit_line(app_spine *sp, const char *line)
{
	if (!line)
		return false;
	size_t n = strlen(line);
	if (n >= CMD_TAIL_MAX)
		return false;

	cmd_hdr h;
	memset(&h, 0, sizeof h);
	h.kind = CMD_HOST_LINE;
	h.tail = (uint32_t)n;
	return submit(sp, &h, line, n);
}

bool app_submit_cmd(app_spine *sp, const ardop_host_cmd *cmd)
{
	if (!cmd)
		return false;

	/* SEND_DATA carries a borrowed pointer and no admission control. Letting
	 * it through here would bypass the credit and reintroduce the silent
	 * truncation the credit exists to prevent. */
	if (cmd->kind == ARDOP_CMD_SEND_DATA)
		return false;

	cmd_hdr h;
	memset(&h, 0, sizeof h);
	h.kind = CMD_HOST_CMD;
	h.cmd = *cmd;
	h.cmd.data = NULL;
	h.cmd.data_len = 0;
	return submit(sp, &h, NULL, 0);
}

bool app_submit_config(app_spine *sp, app_cfg_key key, app_cfg_value value)
{
	cmd_hdr h;
	memset(&h, 0, sizeof h);
	h.kind = CMD_CONFIG;
	h.cfg_key = (uint8_t)key;

	const char *str = NULL;
	switch (key) {
	case APP_CFG_MYCALL:
	case APP_CFG_GRIDSQUARE:
	case APP_CFG_ARQBW:
	case APP_CFG_PROTOCOLMODE:
	case APP_CFG_FECMODE:
		str = value.str;
		if (!str)
			return false;
		break;
	case APP_CFG_FECREPEATS:
	case APP_CFG_BUSYDET:
		h.num = value.num;
		break;
	case APP_CFG_LISTEN:
	case APP_CFG_AUTOBREAK:
	case APP_CFG_FSKONLY:
	case APP_CFG_USE600MODES:
	case APP_CFG_ENABLEPINGACK:
		h.num = value.flag ? 1 : 0;
		break;
	}

	size_t n = str ? strlen(str) : 0;
	if (n >= APP_CFG_STR_MAX)
		return false;
	h.tail = (uint32_t)n;
	return submit(sp, &h, str, n);
}

/* --- submitter: upward ----------------------------------------------------- */

bool app_events_pop(app_spine *sp, app_event *out)
{
	size_t n = app_ring_read(&sp->eventq, sp->ev_rec, sizeof sp->ev_rec);
	if (n < sizeof(ev_hdr))
		return false;

	ev_hdr h;
	memcpy(&h, sp->ev_rec, sizeof h);

	memset(out, 0, sizeof *out);
	out->kind = (app_event_kind)h.kind;
	out->code = (int)h.code;
	out->flag = h.flag != 0;
	memcpy(out->tag, h.tag, sizeof out->tag);
	out->tag[sizeof out->tag - 1] = '\0';

	const uint8_t *tail = sp->ev_rec + sizeof h;
	size_t text_len = h.text_len;
	if (text_len > APP_TEXT_MAX - 1)
		text_len = APP_TEXT_MAX - 1;
	memcpy(out->text, tail, text_len);
	out->text[text_len] = '\0';

	size_t data_len = h.data_len;
	if (data_len > ARDOP_DEMOD_MAX_PAYLOAD)
		data_len = ARDOP_DEMOD_MAX_PAYLOAD;
	memcpy(out->data, tail + h.text_len, data_len);
	out->data_len = data_len;
	return true;
}

bool app_events_pending(app_spine *sp)
{
	return app_ring_peek(&sp->eventq) != 0;
}

bool app_display_pop(app_spine *sp, ardop_tlm_decoded *out)
{
	size_t n = app_ring_read(&sp->displayq, sp->disp_rec,
				 sizeof sp->disp_rec);
	if (!n)
		return false;

	size_t consumed = 0;
	return ardop_tlm_parse(sp->disp_rec, n, out, &consumed);
}

void app_snapshot(app_spine *sp, app_status *out)
{
	memset(out, 0, sizeof *out);
	out->tnc_attached = atomic_load_explicit(&sp->tnc_attached,
						 memory_order_acquire);
	out->run = sp->run;
	out->fault = sp->fault;
	out->tx_credit = app_tx_credit(sp);
	out->tx_reason = (app_tx_status)atomic_load_explicit(
		&sp->tx_reason, memory_order_relaxed);
	out->display_drops = atomic_load_explicit(&sp->display_drops,
						  memory_order_relaxed);
	out->event_lost = atomic_load_explicit(&sp->event_lost,
					       memory_order_relaxed);
	out->event_used = app_ring_used(&sp->eventq);
	out->event_cap = sp->eventq.cap;
	out->display_used = app_ring_used(&sp->displayq);
	out->display_cap = sp->displayq.cap;
}
