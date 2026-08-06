#include "shell/capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/sys.h"

/**
 * @file capture.c
 * @brief The session-capture wire format and its file (see capture.h).
 *
 * Encoding is explicit byte-at-a-time little-endian, not a struct copy, for
 * the same reason as `shell/telemetry.c`: the file may be read back by a
 * different compiler on a different architecture, possibly years later, so
 * layout and endianness are the format's business rather than the ABI's.
 *
 * No platform split: this is ordinary buffered file I/O (`fopen`/`fwrite`),
 * not device access, and needs none on POSIX or Windows.
 */

struct ardop_capture {
	FILE *f;
};

/* --- little-endian scalar writers ------------------------------------------ */

static void put_u8(uint8_t **p, uint8_t v)
{
	*(*p)++ = v;
}

static void put_u16(uint8_t **p, uint16_t v)
{
	*(*p)++ = (uint8_t)(v & 0xFFu);
	*(*p)++ = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t **p, uint32_t v)
{
	*(*p)++ = (uint8_t)(v & 0xFFu);
	*(*p)++ = (uint8_t)((v >> 8) & 0xFFu);
	*(*p)++ = (uint8_t)((v >> 16) & 0xFFu);
	*(*p)++ = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_i16(uint8_t **p, int16_t v)
{
	put_u16(p, (uint16_t)v);
}

static void put_f32(uint8_t **p, float v)
{
	uint32_t bits;
	memcpy(&bits, &v, sizeof(bits));
	put_u32(p, bits);
}

static void put_bytes(uint8_t **p, const uint8_t *src, size_t n)
{
	memcpy(*p, src, n);
	*p += n;
}

/* --- little-endian scalar readers ------------------------------------------ */

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
	       | ((uint32_t)p[3] << 24);
}

static int16_t get_i16(const uint8_t *p)
{
	return (int16_t)get_u16(p);
}

static float get_f32(const uint8_t *p)
{
	uint32_t bits = get_u32(p);
	float v;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

/* Bound n to what fits in cap bytes, so a caller's overlong string or payload
 * is truncated rather than refused -- see capture.h's ARDOP_CAPTURE_MAX_PAYLOAD
 * comment. */
static uint16_t clamp16(size_t n, size_t cap)
{
	if (n > cap)
		n = cap;
	if (n > 0xFFFFu)
		n = 0xFFFFu;
	return (uint16_t)n;
}

static uint8_t clamp8(size_t n, size_t cap)
{
	if (n > cap)
		n = cap;
	if (n > 0xFFu)
		n = 0xFFu;
	return (uint8_t)n;
}

/* --- pure: encode ----------------------------------------------------------- */

size_t ardop_capture_encode_frame(uint8_t *out, size_t cap,
				  ardop_capture_kind kind, uint8_t frame_type,
				  int16_t quality, int16_t sn,
				  uint16_t bandwidth_hz, const uint8_t *payload,
				  uint16_t payload_len)
{
	size_t fixed = ARDOP_CAPTURE_PREFIX_LEN + 9u;   /* type,q,sn,bw,len */
	if (cap < fixed)
		return 0;
	uint16_t n = clamp16(payload_len, cap - fixed);

	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)kind);
	put_u8(&p, frame_type);
	put_i16(&p, quality);
	put_i16(&p, sn);
	put_u16(&p, bandwidth_hz);
	put_u16(&p, n);
	if (n)
		put_bytes(&p, payload, n);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_leader(uint8_t *out, size_t cap, float offset_hz,
				   int16_t sn)
{
	size_t need = ARDOP_CAPTURE_PREFIX_LEN + 6u;
	if (cap < need)
		return 0;
	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_LEADER);
	put_f32(&p, offset_hz);
	put_i16(&p, sn);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_state(uint8_t *out, size_t cap, uint8_t link_state,
				  const char *remote)
{
	size_t fixed = ARDOP_CAPTURE_PREFIX_LEN + 2u;   /* state, remote_len */
	if (cap < fixed)
		return 0;
	if (!remote)
		remote = "";
	uint8_t n = clamp8(strlen(remote), cap - fixed);

	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_STATE);
	put_u8(&p, link_state);
	put_u8(&p, n);
	if (n)
		put_bytes(&p, (const uint8_t *)remote, n);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_ptt(uint8_t *out, size_t cap, bool key)
{
	size_t need = ARDOP_CAPTURE_PREFIX_LEN + 1u;
	if (cap < need)
		return 0;
	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_PTT);
	put_u8(&p, key ? 1u : 0u);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_bandwidth(uint8_t *out, size_t cap,
				      uint16_t bandwidth_hz)
{
	size_t need = ARDOP_CAPTURE_PREFIX_LEN + 2u;
	if (cap < need)
		return 0;
	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_BANDWIDTH);
	put_u16(&p, bandwidth_hz);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_busy(uint8_t *out, size_t cap, bool busy)
{
	size_t need = ARDOP_CAPTURE_PREFIX_LEN + 1u;
	if (cap < need)
		return 0;
	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_BUSY);
	put_u8(&p, busy ? 1u : 0u);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_rx_data(uint8_t *out, size_t cap, const char *tag,
				    const uint8_t *data, uint16_t data_len)
{
	size_t fixed = ARDOP_CAPTURE_PREFIX_LEN + 3u;   /* tag_len, data_len */
	if (cap < fixed)
		return 0;
	if (!tag)
		tag = "";
	uint8_t tag_len = clamp8(strlen(tag), cap - fixed);
	uint16_t n = clamp16(data_len, cap - fixed - tag_len);

	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_RX_DATA);
	put_u8(&p, tag_len);
	if (tag_len)
		put_bytes(&p, (const uint8_t *)tag, tag_len);
	put_u16(&p, n);
	if (n)
		put_bytes(&p, data, n);
	return (size_t)(p - out);
}

size_t ardop_capture_encode_host_msg(uint8_t *out, size_t cap,
				     const char *text)
{
	size_t fixed = ARDOP_CAPTURE_PREFIX_LEN + 2u;   /* text_len */
	if (cap < fixed)
		return 0;
	if (!text)
		text = "";
	uint16_t n = clamp16(strlen(text), cap - fixed);

	uint8_t *p = out;
	put_u8(&p, ARDOP_CAPTURE_MAGIC);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_VERSION);
	put_u8(&p, (uint8_t)ARDOP_CAPTURE_HOST_MSG);
	put_u16(&p, n);
	if (n)
		put_bytes(&p, (const uint8_t *)text, n);
	return (size_t)(p - out);
}

/* --- pure: decode ------------------------------------------------------------ */

bool ardop_capture_parse_record(const uint8_t *buf, size_t avail,
				ardop_capture_record *out)
{
	if (avail < ARDOP_CAPTURE_PREFIX_LEN)
		return false;
	if (buf[0] != ARDOP_CAPTURE_MAGIC)
		return false;
	if (buf[1] != ARDOP_CAPTURE_VERSION)
		return false;

	memset(out, 0, sizeof(*out));
	out->version = buf[1];
	out->kind = (ardop_capture_kind)buf[2];
	out->quality = -1;
	out->sn = -1;

	const uint8_t *p = buf + ARDOP_CAPTURE_PREFIX_LEN;
	size_t left = avail - ARDOP_CAPTURE_PREFIX_LEN;

	switch (out->kind) {
	case ARDOP_CAPTURE_FRAME_RX:
	case ARDOP_CAPTURE_FRAME_TX:
	case ARDOP_CAPTURE_FRAME_RX_FAILED:
		if (left < 9u)
			return false;
		out->frame_type = p[0];
		out->quality = get_i16(p + 1);
		out->sn = get_i16(p + 3);
		out->bandwidth_hz = get_u16(p + 5);
		out->payload_len = get_u16(p + 7);
		if (left < 9u + (size_t)out->payload_len)
			return false;
		out->payload = out->payload_len ? p + 9 : NULL;
		return true;

	case ARDOP_CAPTURE_LEADER:
		if (left < 6u)
			return false;
		out->offset_hz = get_f32(p);
		out->sn = get_i16(p + 4);
		return true;

	case ARDOP_CAPTURE_STATE:
		if (left < 2u)
			return false;
		out->link_state = p[0];
		out->remote_len = p[1];
		if (left < 2u + (size_t)out->remote_len)
			return false;
		out->remote = out->remote_len ? (const char *)(p + 2) : "";
		return true;

	case ARDOP_CAPTURE_PTT:
		if (left < 1u)
			return false;
		out->flag = p[0] != 0;
		return true;

	case ARDOP_CAPTURE_BANDWIDTH:
		if (left < 2u)
			return false;
		out->bandwidth_hz = get_u16(p);
		return true;

	case ARDOP_CAPTURE_BUSY:
		if (left < 1u)
			return false;
		out->flag = p[0] != 0;
		return true;

	case ARDOP_CAPTURE_RX_DATA:
		if (left < 1u)
			return false;
		out->tag_len = p[0];
		if (left < 1u + (size_t)out->tag_len + 2u)
			return false;
		out->tag = out->tag_len ? (const char *)(p + 1) : "";
		out->payload_len = get_u16(p + 1 + out->tag_len);
		if (left < 1u + (size_t)out->tag_len + 2u + (size_t)out->payload_len)
			return false;
		out->payload = out->payload_len
					? p + 1 + out->tag_len + 2
					: NULL;
		return true;

	case ARDOP_CAPTURE_HOST_MSG:
		if (left < 2u)
			return false;
		out->text_len = get_u16(p);
		if (left < 2u + (size_t)out->text_len)
			return false;
		out->text = out->text_len ? (const char *)(p + 2) : "";
		return true;
	}

	return false;   /* unrecognised kind */
}

/* --- impure: the file -------------------------------------------------------- */

/* pcap global header: magic (little-endian marker), version 2.4, no timezone
 * adjustment, no accuracy figure, a snaplen well above any record this format
 * ever writes, and LINKTYPE_USER0 (147) -- one of Wireshark/tcpdump's
 * private-use range, so nothing here collides with a protocol a real
 * dissector already claims. */
static bool write_global_header(FILE *f)
{
	uint8_t hdr[24];
	uint8_t *p = hdr;
	put_u32(&p, 0xa1b2c3d4u);
	put_u16(&p, 2);
	put_u16(&p, 4);
	put_u32(&p, 0);
	put_u32(&p, 0);
	put_u32(&p, 65535);
	put_u32(&p, 147);
	if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
		return false;
	fflush(f);
	return true;
}

ardop_capture *ardop_capture_open(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror("capture: open");
		return NULL;
	}
	if (!write_global_header(f)) {
		fclose(f);
		return NULL;
	}

	ardop_capture *cap = calloc(1, sizeof(*cap));
	if (!cap) {
		fclose(f);
		return NULL;
	}
	cap->f = f;
	return cap;
}

/*
 * Wrap one already-encoded record in a pcap packet and write both. record_len
 * is what ardop_capture_encode_* actually returned, never a computed/assumed
 * size.
 *
 * Flushed on every record, deliberately: a capture exists to explain a session
 * after the fact, which is exactly when ardopb is most likely to have ended
 * abnormally -- a crash, a hang killed with SIGKILL, a lost connection to a
 * remote host. None of those give the process a chance to run its own
 * cleanup, so anything still sitting in stdio's userspace buffer at that point
 * is gone; a single record is a few dozen bytes at most and this call is off
 * the audio path's timing budget (the modem thread, not a callback), so the
 * cost of flushing every time is not one this format needs to economise on.
 */
static void write_packet(ardop_capture *cap, const uint8_t *record,
			 size_t record_len)
{
	if (!cap || record_len == 0)
		return;

	uint64_t ms = ardop_wall_ms();
	uint8_t hdr[16];
	uint8_t *p = hdr;
	put_u32(&p, (uint32_t)(ms / 1000u));
	put_u32(&p, (uint32_t)((ms % 1000u) * 1000u));
	put_u32(&p, (uint32_t)record_len);
	put_u32(&p, (uint32_t)record_len);

	/* A full disk must not take the modem off the air: absorbed, not
	 * reported -- there is no fault channel a debugging aid should be
	 * allowed to raise. */
	if (fwrite(hdr, 1, sizeof(hdr), cap->f) != sizeof(hdr))
		return;
	if (fwrite(record, 1, record_len, cap->f) != record_len)
		return;
	fflush(cap->f);
}

void ardop_capture_write_frame(ardop_capture *cap, ardop_capture_kind kind,
			       uint8_t frame_type, int16_t quality, int16_t sn,
			       uint16_t bandwidth_hz, const uint8_t *payload,
			       uint16_t payload_len)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_frame(buf, sizeof(buf), kind, frame_type,
					      quality, sn, bandwidth_hz,
					      payload, payload_len);
	write_packet(cap, buf, n);
}

void ardop_capture_write_leader(ardop_capture *cap, float offset_hz,
				int16_t sn)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_leader(buf, sizeof(buf), offset_hz, sn);
	write_packet(cap, buf, n);
}

void ardop_capture_write_state(ardop_capture *cap, uint8_t link_state,
			       const char *remote)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_state(buf, sizeof(buf), link_state,
					      remote);
	write_packet(cap, buf, n);
}

void ardop_capture_write_ptt(ardop_capture *cap, bool key)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_ptt(buf, sizeof(buf), key);
	write_packet(cap, buf, n);
}

void ardop_capture_write_bandwidth(ardop_capture *cap, uint16_t bandwidth_hz)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_bandwidth(buf, sizeof(buf), bandwidth_hz);
	write_packet(cap, buf, n);
}

void ardop_capture_write_busy(ardop_capture *cap, bool busy)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_busy(buf, sizeof(buf), busy);
	write_packet(cap, buf, n);
}

void ardop_capture_write_rx_data(ardop_capture *cap, const char *tag,
				 const uint8_t *data, size_t data_len)
{
	if (!cap)
		return;
	uint16_t n16 = data_len > 0xFFFFu ? 0xFFFFu : (uint16_t)data_len;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_rx_data(buf, sizeof(buf), tag, data, n16);
	write_packet(cap, buf, n);
}

void ardop_capture_write_host_msg(ardop_capture *cap, const char *text)
{
	if (!cap)
		return;
	uint8_t buf[ARDOP_CAPTURE_MAX_RECORD];
	size_t n = ardop_capture_encode_host_msg(buf, sizeof(buf), text);
	write_packet(cap, buf, n);
}

void ardop_capture_close(ardop_capture *cap)
{
	if (!cap)
		return;
	if (cap->f)
		fclose(cap->f);
	free(cap);
}
