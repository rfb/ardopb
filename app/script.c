#include "app/script.h"

#include "app/asp_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/link/link.h"

/**
 * @file script.c
 * @brief The scripted driver (see script.h).
 */

#define MAX_LINES 512
#define LINE_MAX 512

/* The most a script may send or check in one run. A megabyte is roughly half an
 * hour of air time at ARDOP's rates, which is far longer than any script worth
 * writing; the limit exists so the buffers can be allocated once. */
#define XFER_MAX (1024u * 1024u)

/* Modem samples per second. The clock every timeout is measured against. */
#define SAMPLE_RATE 12000u

/* No `@wait` runs forever: a script that hangs in CI is worse than one that
 * fails, because it takes the whole job's timeout to say so. */
#define DEFAULT_TIMEOUT_SECS 60u

struct app_script {
	char lines[MAX_LINES][LINE_MAX];
	int n_lines;
	int pc;

	app_spine *side[2];

	uint64_t now;        /* elapsed samples */
	uint64_t deadline;   /* for the directive in progress */
	bool started;        /* the current directive has begun waiting */

	/* @send / @sendfile in progress. */
	uint8_t *tx;
	size_t tx_len;      /* bytes this @send wants to place */
	size_t tx_done;     /* bytes accepted so far */
	size_t tx_total;    /* bytes placed across the whole script */

	/* What the peer received, for @expect rx. */
	uint8_t *rx;
	size_t rx_len;
	bool rx_overflow;

	/* Matching for @wait host=, and the progress mark for @wait rx=. */
	char want[LINE_MAX];
	bool matched;
	size_t wait_mark;

	/* The last line printed, so a run of identical notifications -- the
	 * "BUFFER 16384" a saturated transmit queue emits on every offer -- is
	 * reported once with a count rather than a thousand times. */
	char last_print[APP_TEXT_MAX];
	unsigned repeats;

	/* @stress display. */
	uint64_t stress_until;

	/* ASP, when the script asked for it. */
	bool asp_on;
	asp_app asp[2];
	char asp_expect[256];   /* @asp-wait file=NAME */
	bool asp_arrived;
	int asp_texts;
	char asp_last_text[256];

	bool failed;
	bool done;
};

/* --- diagnostics ----------------------------------------------------------- */

static void fail(app_script *sc, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "script:%d: ", sc->pc + 1);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	sc->failed = true;
	sc->done = true;
}

/* --- construction ---------------------------------------------------------- */

app_script *app_script_open(const char *path, app_spine *a, app_spine *b)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
	if (!f) {
		fprintf(stderr, "script: cannot open %s\n", path);
		return NULL;
	}

	app_script *sc = calloc(1, sizeof *sc);
	if (!sc) {
		if (f != stdin)
			fclose(f);
		return NULL;
	}
	sc->side[0] = a;
	sc->side[1] = b;
	sc->tx = malloc(XFER_MAX);
	sc->rx = malloc(XFER_MAX);
	if (!sc->tx || !sc->rx) {
		if (f != stdin)
			fclose(f);
		app_script_close(sc);
		return NULL;
	}

	char buf[LINE_MAX];
	while (fgets(buf, sizeof buf, f)) {
		char *s = buf;
		while (*s == ' ' || *s == '\t')
			s++;
		size_t n = strlen(s);
		while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
			     s[n - 1] == ' ' || s[n - 1] == '\t'))
			s[--n] = '\0';
		if (n == 0 || s[0] == '#')
			continue;
		if (sc->n_lines >= MAX_LINES) {
			fprintf(stderr, "script: more than %d lines\n",
				MAX_LINES);
			break;
		}
		snprintf(sc->lines[sc->n_lines++], LINE_MAX, "%s", s);
	}
	if (f != stdin)
		fclose(f);
	return sc;
}

void app_script_close(app_script *sc)
{
	if (!sc)
		return;
	free(sc->tx);
	free(sc->rx);
	free(sc);
}

bool app_script_failed(const app_script *sc)
{
	return sc->failed;
}

bool app_script_draining_display(const app_script *sc)
{
	return sc->now >= sc->stress_until;
}

void app_script_tick(app_script *sc, uint64_t elapsed)
{
	sc->now = elapsed;
}

/* Print one line, folding an immediate repeat into a count. */
static void show(app_script *sc, int side, const char *text)
{
	char line[APP_TEXT_MAX];
	snprintf(line, sizeof line, "[%c] %s", side ? 'b' : 'a', text);

	if (strcmp(line, sc->last_print) == 0) {
		sc->repeats++;
		return;
	}
	if (sc->repeats)
		printf("    ... x%u\n", sc->repeats + 1);
	sc->repeats = 0;
	snprintf(sc->last_print, sizeof sc->last_print, "%s", line);
	printf("%s\n", line);
}

void app_script_event(app_script *sc, int side, const app_event *ev)
{
	switch (ev->kind) {
	case APP_EV_HOST_MSG:
	case APP_EV_REPLY:
	case APP_EV_FAULT:
		show(sc, side, ev->text);
		if (side == 0 && sc->want[0] && strstr(ev->text, sc->want))
			sc->matched = true;
		break;

	case APP_EV_RX_DATA:
		/* With ASP running the payload is protocol, not raw bytes, so
		 * it goes to the session rather than into the comparison
		 * buffer -- the files on disk are what gets compared. */
		if (sc->asp_on) {
			asp_app_rx(&sc->asp[side], ev->tag, ev->data,
				   ev->data_len);
			break;
		}
		/* Only the peer's receptions are collected: the script sends
		 * from side a and checks what arrived at side b. */
		if (side == 1) {
			if (sc->rx_len + ev->data_len <= XFER_MAX) {
				memcpy(sc->rx + sc->rx_len, ev->data,
				       ev->data_len);
				sc->rx_len += ev->data_len;
			} else {
				sc->rx_overflow = true;
			}
		}
		break;

	case APP_EV_OWNER:
	case APP_EV_DEVICE:
	case APP_EV_GUEST:
		show(sc, side, ev->text);
		break;

	case APP_EV_STATE:
		/*
		 * Shown only when there is a station on the other end.
		 *
		 * Every state change emits one of these, including the return to
		 * DISC, and a transcript with a bare line per transition is one
		 * nobody reads. The remote callsign is the part that is not
		 * already visible in the host messages around it.
		 */
		if (ev->text[0]) {
			char line[APP_TEXT_MAX + 16];
			snprintf(line, sizeof line, "peer %s", ev->text);
			show(sc, side, line);
		}
		break;
	}
}

/* --- ASP ------------------------------------------------------------------- */

/*
 * The harness's own view of an ASP session.
 *
 * Deliberately thin: everything interesting is asserted by the *files on disk*
 * and by test_asp.c. What this adds is that the bytes went through a real
 * modulator, a real demodulator and real ARQ retries on the way.
 */
static void asp_note(void *ctx, const char *text)
{
	app_script *sc = ctx;
	show(sc, 0, text);
}

static void asp_text(void *ctx, const char *text, size_t len, bool raw)
{
	app_script *sc = ctx;
	char line[APP_TEXT_MAX];
	size_t n = len < sizeof line - 8 ? len : sizeof line - 8;
	snprintf(line, sizeof line, "%s\"%.*s\"", raw ? "raw " : "text ",
		 (int)n, text);
	sc->asp_texts++;
	snprintf(sc->asp_last_text, sizeof sc->asp_last_text, "%.*s", (int)n,
		 text);
	show(sc, 1, line);
}

static void asp_offered(void *ctx, const asp_offer *o, const char *safe_name,
		      uint32_t resumable)
{
	app_script *sc = ctx;
	char line[APP_TEXT_MAX];
	/* Both names bounded explicitly: a peer-supplied name is up to 255 bytes
	 * and two of them plus the text do not fit, which the compiler is right
	 * to insist on. */
	snprintf(line, sizeof line,
		 "offered \"%.90s\" as \"%.90s\", %u bytes, holding %u",
		 o->name, safe_name, (unsigned)o->size, (unsigned)resumable);
	show(sc, 1, line);
}

static void asp_done(void *ctx, bool inbound, asp_result_code result,
		     const char *path)
{
	app_script *sc = ctx;
	char line[APP_TEXT_MAX];
	snprintf(line, sizeof line, "%s transfer finished, result %d%s%s",
		 inbound ? "inbound" : "outbound", (int)result,
		 path ? " -> " : "", path ? path : "");
	show(sc, inbound ? 1 : 0, line);
	if (inbound && result == ASP_RESULT_OK)
		sc->asp_arrived = true;
}

static void asp_link(void *ctx, asp_link_state state, const char *peer_call)
{
	app_script *sc = ctx;
	char line[APP_TEXT_MAX];
	snprintf(line, sizeof line, "ASP link %s with %s",
		 state == ASP_LINK_ASP ? "up" : "raw",
		 peer_call[0] ? peer_call : "(unnamed)");
	show(sc, 1, line);
}

bool app_script_asp(app_script *sc, const char *call_a, const char *dir_a,
		    const char *call_b, const char *dir_b)
{
	/* Side b auto-accepts: a script is unattended, and the prompt path is
	 * what test_asp.c covers. */
	static asp_app_hooks hooks_a, hooks_b;
	hooks_a = (asp_app_hooks){.ctx = sc, .note = asp_note,
				  .text_arrived = asp_text,
				  .transfer_done = asp_done,
				  .link_changed = asp_link};
	hooks_b = hooks_a;
	hooks_b.offer_arrived = asp_offered;

	if (!asp_app_open(&sc->asp[0], sc->side[0], call_a, dir_a, &hooks_a))
		return false;
	if (sc->side[1] &&
	    !asp_app_open(&sc->asp[1], sc->side[1], call_b, dir_b, &hooks_b))
		return false;
	sc->asp[1].auto_accept = true;
	sc->asp_on = true;
	return true;
}

void app_script_asp_service(app_script *sc)
{
	if (!sc->asp_on)
		return;
	asp_app_service(&sc->asp[0]);
	if (sc->side[1])
		asp_app_service(&sc->asp[1]);
}

/* --- directive helpers ----------------------------------------------------- */

/* Split off the next whitespace-delimited word. Returns NULL at the end. */
static char *word(char **p)
{
	char *s = *p;
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '\0')
		return NULL;
	char *start = s;
	while (*s && *s != ' ' && *s != '\t')
		s++;
	if (*s)
		*s++ = '\0';
	*p = s;
	return start;
}

/* Parse "10s", "10" or "500ms" into samples. 0 means "not a duration". */
static uint64_t duration_samples(const char *s)
{
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if (end == s)
		return 0;
	if (strcmp(end, "ms") == 0)
		return (uint64_t)v * SAMPLE_RATE / 1000u;
	return (uint64_t)v * SAMPLE_RATE;
}

/* Begin a wait: arm the deadline once, on the first pump of this directive. */
static void arm(app_script *sc, const char *spec)
{
	if (sc->started)
		return;
	sc->started = true;
	uint64_t secs = spec ? duration_samples(spec) : 0;
	sc->deadline = sc->now +
		       (secs ? secs : DEFAULT_TIMEOUT_SECS * SAMPLE_RATE);
}

static bool expired(const app_script *sc)
{
	return sc->now >= sc->deadline;
}

/* Advance to the next line and clear per-directive state. */
static void advance(app_script *sc)
{
	sc->pc++;
	sc->started = false;
	sc->matched = false;
	sc->want[0] = '\0';
}

static void print_stats(app_script *sc)
{
	for (int i = 0; i < 2; i++) {
		if (!sc->side[i])
			continue;
		app_status st;
		app_snapshot(sc->side[i], &st);
		printf("[%c] credit=%zu (%s) tnc=%s events=%zu/%zu lost=%llu "
		       "display=%zu/%zu dropped=%llu\n",
		       i ? 'b' : 'a', st.tx_credit,
		       app_tx_status_str(st.tx_reason),
		       st.tnc_attached ? "yes" : "no", st.event_used,
		       st.event_cap, (unsigned long long)st.event_lost,
		       st.display_used, st.display_cap,
		       (unsigned long long)st.display_drops);
	}
	printf("[.] sent=%zu received=%zu\n", sc->tx_total, sc->rx_len);
}

/* --- @send ----------------------------------------------------------------- */

/* Fill the staging buffer with a deterministic pattern, so a mismatch at the far
 * end names the offset that went wrong rather than just "differs". */
static void generate(app_script *sc, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sc->tx[i] = (uint8_t)((sc->tx_total + i) * 31u + 7u);
}

/* Offer as much of the pending transfer as the credit allows.
 *
 * This is the backpressure demonstration: the loop never blocks, never sleeps
 * and never assumes an offer will be taken whole. It advances its own cursor by
 * whatever was accepted and comes back on the next step -- which is exactly the
 * contract a user interface's frame timer will follow. */
static bool pump_send(app_script *sc)
{
	while (sc->tx_done < sc->tx_len) {
		app_tx_status why = APP_TX_OK;
		size_t took = app_tx_submit(sc->side[0],
					    sc->tx + sc->tx_done,
					    sc->tx_len - sc->tx_done, &why);
		if (took == 0) {
			if (expired(sc)) {
				fail(sc, "@send stalled at %zu/%zu bytes: %s",
				     sc->tx_done, sc->tx_len,
				     app_tx_status_str(why));
				return false;
			}
			return false;   /* not now; try again next step */
		}
		sc->tx_done += took;

		/* The deadline measures a *stall*, not the transfer. A megabyte
		 * over a 2000 Hz link is legitimately an hour of air time, and a
		 * timeout that could not tell that apart from a wedged link
		 * would be useless for both. */
		sc->deadline = sc->now + DEFAULT_TIMEOUT_SECS * SAMPLE_RATE;
	}

	sc->tx_total += sc->tx_len;
	return true;
}

/* --- the directives -------------------------------------------------------- */

/* Run one directive. Returns true when it is finished and the program counter
 * should advance. */
static bool directive(app_script *sc, char *rest)
{
	char *verb = word(&rest);
	if (!verb) {
		fail(sc, "empty directive");
		return false;
	}

	if (strcmp(verb, "echo") == 0) {
		printf("[.] %s\n", rest);
		return true;
	}

	if (strcmp(verb, "stats") == 0) {
		print_stats(sc);
		return true;
	}

	if (strcmp(verb, "peer") == 0) {
		if (!sc->side[1]) {
			fail(sc, "@peer needs --loopback");
			return false;
		}
		if (!app_submit_line(sc->side[1], rest)) {
			fail(sc, "@peer: command queue full");
			return false;
		}
		return true;
	}

	if (strcmp(verb, "send") == 0 || strcmp(verb, "sendfile") == 0) {
		if (!sc->started) {
			if (strcmp(verb, "send") == 0) {
				char *n = word(&rest);
				unsigned long want = n ? strtoul(n, NULL, 10)
						       : 0;
				if (want == 0 || want > XFER_MAX) {
					fail(sc, "@send needs 1..%u bytes",
					     XFER_MAX);
					return false;
				}
				sc->tx_len = want;
				generate(sc, want);
			} else {
				char *path = word(&rest);
				FILE *f = path ? fopen(path, "rb") : NULL;
				if (!f) {
					fail(sc, "@sendfile: cannot open %s",
					     path ? path : "(none)");
					return false;
				}
				sc->tx_len = fread(sc->tx, 1, XFER_MAX, f);
				fclose(f);
				if (sc->tx_len == 0) {
					fail(sc, "@sendfile: %s is empty",
					     path);
					return false;
				}
			}
			sc->tx_done = 0;
			arm(sc, NULL);
		}
		return pump_send(sc);
	}

	if (strcmp(verb, "asp-send") == 0) {
		if (!sc->asp_on) {
			fail(sc, "@asp-send needs --asp");
			return false;
		}
		if (!sc->started) {
			char *path = word(&rest);
			if (!path || !asp_app_send_file(&sc->asp[0], path)) {
				fail(sc, "@asp-send: cannot offer %s",
				     path ? path : "(none)");
				return false;
			}
			sc->asp_arrived = false;
			arm(sc, NULL);
		}
		return true;
	}

	if (strcmp(verb, "asp-text") == 0) {
		if (!sc->asp_on) {
			fail(sc, "@asp-text needs --asp");
			return false;
		}
		while (*rest == ' ')
			rest++;
		if (!asp_app_send_text(&sc->asp[0], rest)) {
			fail(sc, "@asp-text: refused");
			return false;
		}
		return true;
	}

	if (strcmp(verb, "asp-wait") == 0) {
		if (!sc->asp_on) {
			fail(sc, "@asp-wait needs --asp");
			return false;
		}
		char *what = word(&rest);
		if (!what) {
			fail(sc, "@asp-wait needs a condition");
			return false;
		}
		arm(sc, word(&rest));

		if (strncmp(what, "link", 4) == 0) {
			/*
			 * Both HELLOs exchanged. Worth its own condition because
			 * it is not instant on a real link: the answering
			 * station is the IRS and cannot send until it gets a
			 * turn, which is what AUTOBREAK is for (analysis/17 §7).
			 * A script that offered a file before this would be
			 * refused, and refused for a reason that looks like a
			 * bug rather than like timing.
			 */
			if (sc->asp[0].session.state == ASP_LINK_ASP &&
			    (!sc->side[1] ||
			     sc->asp[1].session.state == ASP_LINK_ASP))
				return true;
			if (expired(sc)) {
				fail(sc, "@asp-wait link timed out (a=%d b=%d)",
				     (int)sc->asp[0].session.state,
				     (int)sc->asp[1].session.state);
				return false;
			}
			return false;
		}
		if (strncmp(what, "file", 4) == 0) {
			if (sc->asp_arrived)
				return true;
			if (expired(sc)) {
				fail(sc, "@asp-wait file timed out");
				return false;
			}
			return false;
		}
		if (strncmp(what, "text=", 5) == 0) {
			if (strstr(sc->asp_last_text, what + 5) != NULL)
				return true;
			if (expired(sc)) {
				fail(sc, "@asp-wait text=%s timed out",
				     what + 5);
				return false;
			}
			return false;
		}
		fail(sc, "@asp-wait: unknown condition %s", what);
		return false;
	}

	if (strcmp(verb, "wait") == 0) {
		char *what = word(&rest);
		if (!what) {
			fail(sc, "@wait needs a condition");
			return false;
		}

		if (strncmp(what, "secs=", 5) == 0) {
			if (!sc->started) {
				sc->started = true;
				sc->deadline = sc->now +
					       duration_samples(what + 5);
			}
			return expired(sc);
		}

		if (strncmp(what, "host=", 5) == 0) {
			if (!sc->started)
				snprintf(sc->want, sizeof sc->want, "%s",
					 what + 5);
			arm(sc, word(&rest));
			if (sc->matched)
				return true;
			if (expired(sc)) {
				fail(sc, "@wait host=%s timed out", sc->want);
				return false;
			}
			return false;
		}

		if (strncmp(what, "rx=", 3) == 0) {
			if (!sc->side[1]) {
				fail(sc, "@wait rx= needs --loopback");
				return false;
			}
			arm(sc, word(&rest));
			size_t want = (size_t)strtoul(what + 3, NULL, 10);
			if (sc->rx_len >= want)
				return true;
			/* Like @send, the deadline is for a stall rather than
			 * the whole transfer: a frame arriving means the link is
			 * alive, whatever the total is going to take. */
			if (sc->rx_len != sc->wait_mark) {
				sc->wait_mark = sc->rx_len;
				sc->deadline = sc->now +
					       DEFAULT_TIMEOUT_SECS * SAMPLE_RATE;
			}
			if (expired(sc)) {
				fail(sc, "@wait rx=%zu timed out at %zu",
				     want, sc->rx_len);
				return false;
			}
			return false;
		}

		fail(sc, "@wait: unknown condition %s", what);
		return false;
	}

	if (strcmp(verb, "expect") == 0) {
		char *what = word(&rest);
		if (!what) {
			fail(sc, "@expect needs something to check");
			return false;
		}

		if (strcmp(what, "rx") == 0) {
			if (!sc->side[1]) {
				fail(sc, "@expect rx needs --loopback");
				return false;
			}
			if (sc->rx_overflow) {
				fail(sc, "@expect rx: more than %u bytes "
					 "received", XFER_MAX);
				return false;
			}
			if (sc->rx_len != sc->tx_total) {
				fail(sc, "@expect rx: sent %zu, received %zu",
				     sc->tx_total, sc->rx_len);
				return false;
			}
			/* Regenerate rather than keep every byte sent: the
			 * pattern is a pure function of the offset. */
			for (size_t i = 0; i < sc->rx_len; i++) {
				uint8_t want = (uint8_t)(i * 31u + 7u);
				if (sc->rx[i] != want) {
					fail(sc, "@expect rx: byte %zu is "
						 "0x%02x, expected 0x%02x",
					     i, sc->rx[i], want);
					return false;
				}
			}
			printf("[.] %zu bytes verified\n", sc->rx_len);
			return true;
		}

		if (strcmp(what, "no-loss") == 0) {
			for (int i = 0; i < 2; i++) {
				if (!sc->side[i])
					continue;
				app_status st;
				app_snapshot(sc->side[i], &st);
				if (st.event_lost) {
					fail(sc, "@expect no-loss: side %c lost "
						 "%llu event(s)",
					     i ? 'b' : 'a',
					     (unsigned long long)st.event_lost);
					return false;
				}
			}
			printf("[.] no events lost\n");
			return true;
		}

		fail(sc, "@expect: unknown check %s", what);
		return false;
	}

	if (strcmp(verb, "stress") == 0) {
		char *which = word(&rest);
		char *dur = word(&rest);
		if (!which || strcmp(which, "display") != 0 || !dur) {
			fail(sc, "usage: @stress display <duration>");
			return false;
		}
		if (!sc->started) {
			sc->started = true;
			sc->stress_until = sc->now + duration_samples(dur);
		}
		if (sc->now < sc->stress_until)
			return false;

		/* The point of the exercise: a display that stopped being read
		 * loses rows and says so, and costs the lossless queue nothing. */
		app_status st;
		app_snapshot(sc->side[0], &st);
		if (st.display_drops == 0) {
			fail(sc, "@stress display: nothing was dropped");
			return false;
		}
		if (st.event_lost) {
			fail(sc, "@stress display: %llu event(s) lost",
			     (unsigned long long)st.event_lost);
			return false;
		}
		printf("[.] display dropped %llu record(s), no event lost\n",
		       (unsigned long long)st.display_drops);
		return true;
	}

	fail(sc, "unknown directive @%s", verb);
	return false;
}

bool app_script_pump(app_script *sc)
{
	if (sc->done)
		return false;

	/* Run everything that is ready, stopping at the first directive that has
	 * to wait -- that one resumes on the next call. */
	while (sc->pc < sc->n_lines) {
		char line[LINE_MAX];
		snprintf(line, sizeof line, "%s", sc->lines[sc->pc]);

		if (line[0] == '@') {
			char *rest = line + 1;
			if (!directive(sc, rest)) {
				if (sc->failed)
					return false;
				return true;   /* still waiting */
			}
			advance(sc);
			continue;
		}

		if (!app_submit_line(sc->side[0], line)) {
			/* The ring is full, which for the downward queue means
			 * the modem has not run yet. Retry next step. */
			return true;
		}
		advance(sc);
	}

	sc->done = true;
	return false;
}
