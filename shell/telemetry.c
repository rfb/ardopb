#include "shell/telemetry.h"

#include <string.h>

/**
 * @file telemetry.c
 * @brief The telemetry wire format (see telemetry.h).
 *
 * Encoding is explicit byte-at-a-time little-endian rather than a struct copy:
 * the stream crosses a socket to a process that may have been built by another
 * compiler on another architecture, so layout, padding and endianness must be
 * the format's business rather than the ABI's.
 */

/* --- little-endian scalar writers ---------------------------------------- */

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

/* IEEE-754 single precision is what every platform this runs on uses; the copy
 * moves the bits without reinterpreting them as an integer value. */
static void put_f32(uint8_t **p, float v)
{
	uint32_t bits;
	memcpy(&bits, &v, sizeof(bits));
	put_u32(p, bits);
}

/* --- little-endian scalar readers ---------------------------------------- */

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

/* --- hello ---------------------------------------------------------------- */

size_t ardop_tlm_encode_hello(uint8_t *out)
{
	uint8_t *p = out;

	put_u32(&p, ARDOP_TLM_MAGIC);
	put_u16(&p, (uint16_t)ARDOP_TLM_VERSION);
	put_u16(&p, (uint16_t)ARDOP_BUSY_MAG_BINS);
	put_u16(&p, (uint16_t)ARDOP_BUSY_FIRST_BIN);
	put_u16(&p, 0);   /* reserved, keeps the float 4-byte aligned in the file. */
	put_f32(&p, ARDOP_BUSY_BIN_HZ);
	return (size_t)(p - out);
}

bool ardop_tlm_parse_hello(const uint8_t *buf, size_t avail, uint16_t *bins,
			   uint16_t *first_bin, float *bin_hz)
{
	if (avail < ARDOP_TLM_HELLO_LEN)
		return false;
	if (get_u32(buf) != ARDOP_TLM_MAGIC)
		return false;
	if (get_u16(buf + 4) != ARDOP_TLM_VERSION)
		return false;

	*bins = get_u16(buf + 6);
	*first_bin = get_u16(buf + 8);
	*bin_hz = get_f32(buf + 12);
	return true;
}

/* --- records -------------------------------------------------------------- */

size_t ardop_tlm_encode(const ardop_telemetry *t, uint8_t *out, size_t cap)
{
	uint8_t *p = out + ARDOP_TLM_HEADER_LEN;   /* header filled in last. */
	size_t need;

	switch (t->kind) {
	case ARDOP_TLM_SPECTRUM:
		need = ARDOP_TLM_HEADER_LEN + ARDOP_BUSY_MAG_BINS * 4u;
		if (cap < need || t->mag == NULL)
			return 0;
		for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
			put_f32(&p, t->mag[i]);
		break;

	case ARDOP_TLM_CONSTELLATION: {
		uint16_t n = t->n_points;
		if (n > ARDOP_TLM_MAX_POINTS)
			n = ARDOP_TLM_MAX_POINTS;
		need = ARDOP_TLM_HEADER_LEN + 6u + (size_t)n * 4u;
		if (cap < need || t->phase_mrad == NULL || t->point_mag == NULL)
			return 0;
		put_u8(&p, t->frame_type);
		put_u8(&p, t->modulation);
		put_u16(&p, n);
		put_i16(&p, t->mag_threshold);
		for (uint16_t i = 0; i < n; i++)
			put_i16(&p, t->phase_mrad[i]);
		for (uint16_t i = 0; i < n; i++)
			put_i16(&p, t->point_mag[i]);
		break;
	}

	case ARDOP_TLM_AUDIO:
		need = ARDOP_TLM_HEADER_LEN + 8u;
		if (cap < need)
			return 0;
		put_f32(&p, t->rms);
		put_f32(&p, t->peak);
		break;

	case ARDOP_TLM_STATUS:
		need = ARDOP_TLM_HEADER_LEN + 14u;
		if (cap < need)
			return 0;
		put_u8(&p, t->state);
		put_u8(&p, t->mode);
		put_u8(&p, t->busy ? 1u : 0u);
		put_u8(&p, t->ptt ? 1u : 0u);
		put_i16(&p, t->sn);
		put_i16(&p, t->quality);
		put_i16(&p, t->bandwidth);
		put_u32(&p, t->buffer_len);
		break;

	default:
		return 0;
	}

	/* The header, now that the payload length is known. */
	size_t payload = (size_t)(p - out) - ARDOP_TLM_HEADER_LEN;
	uint8_t *h = out;
	put_u8(&h, (uint8_t)t->kind);
	put_u16(&h, (uint16_t)payload);
	return (size_t)(p - out);
}

bool ardop_tlm_parse(const uint8_t *buf, size_t avail, ardop_tlm_decoded *out,
		     size_t *consumed)
{
	if (avail < ARDOP_TLM_HEADER_LEN)
		return false;

	uint8_t kind = buf[0];
	size_t payload = get_u16(buf + 1);
	size_t total = ARDOP_TLM_HEADER_LEN + payload;

	if (avail < total)
		return false;

	*consumed = total;
	const uint8_t *b = buf + ARDOP_TLM_HEADER_LEN;
	memset(&out->rec, 0, sizeof(out->rec));

	switch (kind) {
	case ARDOP_TLM_SPECTRUM: {
		if (payload != ARDOP_BUSY_MAG_BINS * 4u)
			return false;
		for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
			out->mag[i] = get_f32(b + (size_t)i * 4u);
		out->rec.kind = ARDOP_TLM_SPECTRUM;
		out->rec.mag = out->mag;
		return true;
	}

	case ARDOP_TLM_CONSTELLATION: {
		if (payload < 6u)
			return false;
		uint16_t n = get_u16(b + 2);
		if (n > ARDOP_TLM_MAX_POINTS
		    || payload != 6u + (size_t)n * 4u)
			return false;
		const uint8_t *ph = b + 6;
		const uint8_t *mg = ph + (size_t)n * 2u;
		for (uint16_t i = 0; i < n; i++) {
			out->phase_mrad[i] = get_i16(ph + (size_t)i * 2u);
			out->point_mag[i] = get_i16(mg + (size_t)i * 2u);
		}
		out->rec.kind = ARDOP_TLM_CONSTELLATION;
		out->rec.frame_type = b[0];
		out->rec.modulation = b[1];
		out->rec.n_points = n;
		out->rec.mag_threshold = get_i16(b + 4);
		out->rec.phase_mrad = out->phase_mrad;
		out->rec.point_mag = out->point_mag;
		return true;
	}

	case ARDOP_TLM_AUDIO:
		if (payload != 8u)
			return false;
		out->rec.kind = ARDOP_TLM_AUDIO;
		out->rec.rms = get_f32(b);
		out->rec.peak = get_f32(b + 4);
		return true;

	case ARDOP_TLM_STATUS:
		if (payload != 14u)
			return false;
		out->rec.kind = ARDOP_TLM_STATUS;
		out->rec.state = b[0];
		out->rec.mode = b[1];
		out->rec.busy = b[2] != 0;
		out->rec.ptt = b[3] != 0;
		out->rec.sn = get_i16(b + 4);
		out->rec.quality = get_i16(b + 6);
		out->rec.bandwidth = get_i16(b + 8);
		out->rec.buffer_len = get_u32(b + 10);
		return true;

	default:
		/* Unknown kind: consumed is set, so the reader skips it and a
		 * newer daemon can add records without breaking an old GUI. */
		return false;
	}
}
