#define _DEFAULT_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "codec/frame.h"

#include "shell/capture.h"

/**
 * @file ardop_pcap_dump.c
 * @brief ardop-pcap-dump -- print a captured session as text (see capture.h).
 *
 * The actual "review a session together" surface. Raw hex in Wireshark for a
 * custom linktype isn't practically readable without writing a dissector, so
 * this prints one merged, time-ordered, human-legible line per event instead
 * -- frames and the session's narrative (leader acquisitions, link state,
 * PTT, bandwidth, busy, delivered data, host status) interleaved in the order
 * they happened, which is already the file's own order.
 *
 * The pcap envelope (global header, per-packet header) is parsed here and
 * nowhere else -- capture.c never needs to read its own file back, the same
 * way telemetry.c's ardop_tlm_parse only ever handles one record and leaves
 * stream framing to each consumer.
 */

/* Coarser than the wire's ardop_link_state numbering documents, but this file
 * has no link.h dependency and does not need one for a name lookup this
 * small; kept in sync by hand against core/link/link.h's enum order. */
static const char *const kLinkStateNames[] = {
	"DISC", "ISS_CON_REQ", "ISS_CON_ACK", "ISS", "IRS_CON_ACK",
	"IRS_DATA", "IDLE", "IRS_TO_ISS", "IRS_FROM_ISS", "FEC_SEND",
};
#define N_LINK_STATES ((int)(sizeof kLinkStateNames / sizeof kLinkStateNames[0]))

static const char *state_name(uint8_t s)
{
	return s < N_LINK_STATES ? kLinkStateNames[s] : "?";
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
	       | ((uint32_t)p[3] << 24);
}

/* Print [payload, payload+len) as a quoted printable string if every byte
 * qualifies, else as hex pairs; either way capped so one huge record does not
 * flood the terminal. */
static void print_bytes(const uint8_t *payload, size_t len)
{
	static const size_t kMax = 48;
	bool printable = len > 0;
	for (size_t i = 0; i < len && printable; i++)
		if (!isprint(payload[i]) && payload[i] != ' ')
			printable = false;

	size_t shown = len < kMax ? len : kMax;
	if (printable) {
		printf("\"%.*s\"%s", (int)shown, (const char *)payload,
		       len > shown ? "..." : "");
	} else {
		for (size_t i = 0; i < shown; i++)
			printf("%02x", payload[i]);
		if (len > shown)
			printf("...");
	}
}

static void print_timestamp(uint32_t ts_sec, uint32_t ts_usec)
{
	time_t t = (time_t)ts_sec;
	struct tm tmv;
	gmtime_r(&t, &tmv);
	printf("%02d:%02d:%02d.%03u  ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
	       (unsigned)(ts_usec / 1000u));
}

static void print_record(const ardop_capture_record *r)
{
	switch (r->kind) {
	case ARDOP_CAPTURE_FRAME_RX:
	case ARDOP_CAPTURE_FRAME_TX:
	case ARDOP_CAPTURE_FRAME_RX_FAILED: {
		const ardop_frame_spec *spec = ardop_frame_spec_for(r->frame_type);
		const char *name = spec ? spec->name : "(unknown)";
		if (r->kind == ARDOP_CAPTURE_FRAME_TX) {
			printf("TX        %-22s bw=%uHz  ", name,
			       (unsigned)r->bandwidth_hz);
		} else if (r->kind == ARDOP_CAPTURE_FRAME_RX) {
			printf("RX        %-22s q=%d sn=%d bw=%uHz  ", name,
			       r->quality, r->sn, (unsigned)r->bandwidth_hz);
		} else {
			printf("RX FAIL   %-22s sn=%d  ", name, r->sn);
		}
		if (r->payload_len)
			print_bytes(r->payload, r->payload_len);
		else
			printf("(no payload)");
		printf("\n");
		break;
	}

	case ARDOP_CAPTURE_LEADER:
		printf("LEADER    %+.0f Hz  sn=%d\n", (double)r->offset_hz,
		       r->sn);
		break;

	case ARDOP_CAPTURE_STATE: {
		static bool have_last;
		static uint8_t last_state;
		if (have_last)
			printf("STATE     %s -> %s", state_name(last_state),
			       state_name(r->link_state));
		else
			printf("STATE     %s", state_name(r->link_state));
		have_last = true;
		last_state = r->link_state;
		if (r->remote_len)
			printf("  remote=%.*s", r->remote_len, r->remote);
		printf("\n");
		break;
	}

	case ARDOP_CAPTURE_PTT:
		printf("PTT       %s\n", r->flag ? "key" : "unkey");
		break;

	case ARDOP_CAPTURE_BANDWIDTH:
		printf("BANDWIDTH %u Hz\n", (unsigned)r->bandwidth_hz);
		break;

	case ARDOP_CAPTURE_BUSY:
		printf("BUSY      %s\n", r->flag ? "busy" : "clear");
		break;

	case ARDOP_CAPTURE_RX_DATA:
		printf("RX_DATA   %.*s  %u bytes  ", r->tag_len, r->tag,
		       (unsigned)r->payload_len);
		if (r->payload_len)
			print_bytes(r->payload, r->payload_len);
		printf("\n");
		break;

	case ARDOP_CAPTURE_HOST_MSG:
		printf("HOST_MSG  \"%.*s\"\n", r->text_len, r->text);
		break;
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s FILE.pcap\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror("open");
		return 1;
	}

	uint8_t ghdr[24];
	if (fread(ghdr, 1, sizeof(ghdr), f) != sizeof(ghdr)) {
		fprintf(stderr, "%s: too short to be a pcap file\n", argv[1]);
		fclose(f);
		return 1;
	}
	if (get_u32(ghdr) != 0xa1b2c3d4u) {
		fprintf(stderr, "%s: not a little-endian classic pcap file\n",
			argv[1]);
		fclose(f);
		return 1;
	}
	if (get_u32(ghdr + 20) != 147u) {
		fprintf(stderr,
			"%s: pcap file, but not LINKTYPE_USER0 (147) -- not "
			"an ardop capture\n",
			argv[1]);
		fclose(f);
		return 1;
	}

	size_t count = 0;
	for (;;) {
		uint8_t phdr[16];
		size_t got = fread(phdr, 1, sizeof(phdr), f);
		if (got == 0)
			break;   /* clean end of file, at a packet boundary. */
		if (got != sizeof(phdr)) {
			fprintf(stderr, "%s: truncated mid packet-header at "
					"record %zu\n",
				argv[1], count);
			fclose(f);
			return 1;
		}

		uint32_t ts_sec = get_u32(phdr);
		uint32_t ts_usec = get_u32(phdr + 4);
		uint32_t incl_len = get_u32(phdr + 8);

		static uint8_t rec[ARDOP_CAPTURE_MAX_RECORD];
		if (incl_len > sizeof(rec)) {
			fprintf(stderr, "%s: record %zu implausibly large "
					"(%u bytes), stopping\n",
				argv[1], count, (unsigned)incl_len);
			fclose(f);
			return 1;
		}
		if (fread(rec, 1, incl_len, f) != incl_len) {
			fprintf(stderr, "%s: truncated mid record %zu\n",
				argv[1], count);
			fclose(f);
			return 1;
		}

		ardop_capture_record out;
		if (!ardop_capture_parse_record(rec, incl_len, &out)) {
			fprintf(stderr, "%s: malformed record %zu, skipping "
					"%u bytes\n",
				argv[1], count, (unsigned)incl_len);
			count++;
			continue;
		}

		print_timestamp(ts_sec, ts_usec);
		print_record(&out);
		count++;
	}

	fclose(f);
	return 0;
}
