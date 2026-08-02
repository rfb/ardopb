#include "app/asp_wire.h"

#include <string.h>

/**
 * @file asp_wire.c
 * @brief ASP/1 on the wire (see asp_wire.h).
 */

/* --- varints ---------------------------------------------------------------- */

/* Four bytes carry 28 bits, which is 64 times the largest legal payload. The cap
 * is not about range; it is so that a decoder's worst case is a constant. */
#define ASP_VARINT_MAX 4

size_t asp_varint_put(uint8_t *out, size_t cap, uint32_t v)
{
	size_t n = 0;
	do {
		if (n >= cap || n >= ASP_VARINT_MAX)
			return 0;
		uint8_t byte = (uint8_t)(v & 0x7fu);
		v >>= 7;
		if (v)
			byte |= 0x80u;
		out[n++] = byte;
	} while (v);
	return n;
}

bool asp_varint_get(const uint8_t *buf, size_t avail, uint32_t *v, size_t *used)
{
	uint32_t acc = 0;
	for (size_t i = 0; i < avail && i < ASP_VARINT_MAX; i++) {
		acc |= (uint32_t)(buf[i] & 0x7fu) << (7 * i);
		if (!(buf[i] & 0x80u)) {
			*v = acc;
			*used = i + 1;
			return true;
		}
	}
	/* Either the buffer ran out, or a fifth continuation byte was asked for.
	 * The caller separates those by comparing avail against the cap. */
	return false;
}

/* --- CRC-32 ----------------------------------------------------------------- */

/*
 * The reflected CRC-32 of IEEE 802.3, computed a nibble at a time.
 *
 * Sixteen entries rather than the usual 256: this runs over file payload on a
 * link that moves a few hundred bytes a second, so the byte-at-a-time table's
 * extra speed buys nothing, and a 64-byte table is easier to hold in cache
 * alongside the modem's own tables than a kilobyte one.
 */
static const uint32_t kCrcNibble[16] = {
	0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
	0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
	0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
	0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

uint32_t asp_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		crc = (crc >> 4) ^ kCrcNibble[crc & 0x0fu];
		crc = (crc >> 4) ^ kCrcNibble[crc & 0x0fu];
	}
	return ~crc;
}

/* --- framing ---------------------------------------------------------------- */

size_t asp_frame_put(uint8_t *out, size_t cap, asp_msg_type type,
		     const void *payload, size_t len)
{
	if (len > ASP_MAX_PAYLOAD)
		return 0;
	if (cap < 1)
		return 0;

	out[0] = (uint8_t)type;
	size_t n = asp_varint_put(out + 1, cap - 1, (uint32_t)len);
	if (n == 0)
		return 0;
	if (cap < 1 + n + len)
		return 0;
	if (len && payload)
		memcpy(out + 1 + n, payload, len);
	return 1 + n + len;
}

asp_frame_status asp_frame_get(const uint8_t *buf, size_t avail,
			       asp_msg_type *type, const uint8_t **payload,
			       size_t *len, size_t *consumed)
{
	if (avail < 1)
		return ASP_FRAME_SHORT;

	/* Reserved, and the reason it is reserved: a run of zero bytes is what a
	 * half-open or desynchronised stream most often produces, and it must
	 * never look like a valid message. */
	if (buf[0] == 0x00)
		return ASP_FRAME_ERROR;

	uint32_t plen = 0;
	size_t vn = 0;
	if (!asp_varint_get(buf + 1, avail - 1, &plen, &vn)) {
		/* Short buffer or overlong varint, and the two are different
		 * answers: one waits, the other is fatal. */
		return (avail - 1 < ASP_VARINT_MAX) ? ASP_FRAME_SHORT
						    : ASP_FRAME_ERROR;
	}
	if (plen > ASP_MAX_PAYLOAD)
		return ASP_FRAME_ERROR;

	const size_t total = 1 + vn + plen;
	if (avail < total)
		return ASP_FRAME_SHORT;

	*type = (asp_msg_type)buf[0];
	*payload = buf + 1 + vn;
	*len = plen;
	*consumed = total;
	return ASP_FRAME_OK;
}

/* --- little-endian scalars, matching every other wire format in the tree ----- */

static void put_u16(uint8_t **p, uint16_t v)
{
	(*p)[0] = (uint8_t)(v & 0xffu);
	(*p)[1] = (uint8_t)(v >> 8);
	*p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
	(*p)[0] = (uint8_t)(v & 0xffu);
	(*p)[1] = (uint8_t)((v >> 8) & 0xffu);
	(*p)[2] = (uint8_t)((v >> 16) & 0xffu);
	(*p)[3] = (uint8_t)((v >> 24) & 0xffu);
	*p += 4;
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* A length-prefixed string, u8 length. Returns false if it does not fit. */
static bool put_str(uint8_t **p, const uint8_t *end, const char *s, size_t max)
{
	size_t n = strlen(s);
	if (n > max)
		n = max;
	if (*p + 1 + n > end)
		return false;
	*(*p)++ = (uint8_t)n;
	memcpy(*p, s, n);
	*p += n;
	return true;
}

static bool get_str(const uint8_t **p, const uint8_t *end, char *out,
		    size_t cap)
{
	if (*p >= end)
		return false;
	size_t n = **p;
	(*p)++;
	if (*p + n > end || n >= cap)
		return false;
	memcpy(out, *p, n);
	out[n] = '\0';
	*p += n;
	return true;
}

/* --- payloads --------------------------------------------------------------- */

size_t asp_hello_put(uint8_t *out, size_t cap, const asp_hello *h)
{
	uint8_t *p = out, *end = out + cap;
	if (cap < strlen(ASP_MAGIC) + 6)
		return 0;
	memcpy(p, ASP_MAGIC, strlen(ASP_MAGIC));
	p += strlen(ASP_MAGIC);
	*p++ = h->version;
	put_u32(&p, h->caps);
	if (!put_str(&p, end, h->call, ASP_MAX_CALL - 1))
		return 0;
	return (size_t)(p - out);
}

bool asp_hello_get(const uint8_t *p, size_t len, asp_hello *h)
{
	const size_t magic = strlen(ASP_MAGIC);
	const uint8_t *end = p + len;
	if (len < magic + 6 || memcmp(p, ASP_MAGIC, magic) != 0)
		return false;
	p += magic;
	h->version = *p++;
	h->caps = get_u32(p);
	p += 4;
	return get_str(&p, end, h->call, sizeof h->call);
}

size_t asp_offer_put(uint8_t *out, size_t cap, const asp_offer *o)
{
	uint8_t *p = out, *end = out + cap;
	if (cap < 10)
		return 0;
	put_u16(&p, o->id);
	put_u32(&p, o->size);
	put_u32(&p, o->crc32);
	if (!put_str(&p, end, o->name, ASP_MAX_NAME))
		return 0;
	if (!put_str(&p, end, o->content_type, sizeof o->content_type - 1))
		return 0;
	return (size_t)(p - out);
}

bool asp_offer_get(const uint8_t *p, size_t len, asp_offer *o)
{
	const uint8_t *end = p + len;
	if (len < 10)
		return false;
	o->id = get_u16(p);
	p += 2;
	o->size = get_u32(p);
	p += 4;
	o->crc32 = get_u32(p);
	p += 4;
	if (!get_str(&p, end, o->name, sizeof o->name))
		return false;
	return get_str(&p, end, o->content_type, sizeof o->content_type);
}

size_t asp_accept_put(uint8_t *out, size_t cap, const asp_accept *a)
{
	uint8_t *p = out;
	if (cap < 10)
		return 0;
	put_u16(&p, a->id);
	put_u32(&p, a->have);
	put_u32(&p, a->prefix_crc);
	return (size_t)(p - out);
}

bool asp_accept_get(const uint8_t *p, size_t len, asp_accept *a)
{
	if (len < 10)
		return false;
	a->id = get_u16(p);
	a->have = get_u32(p + 2);
	a->prefix_crc = get_u32(p + 6);
	return true;
}

size_t asp_id_code_put(uint8_t *out, size_t cap, uint16_t id, uint8_t code)
{
	uint8_t *p = out;
	if (cap < 3)
		return 0;
	put_u16(&p, id);
	*p++ = code;
	return (size_t)(p - out);
}

bool asp_id_code_get(const uint8_t *p, size_t len, uint16_t *id, uint8_t *code)
{
	if (len < 3)
		return false;
	*id = get_u16(p);
	*code = p[2];
	return true;
}

size_t asp_start_put(uint8_t *out, size_t cap, uint16_t id, uint32_t from)
{
	uint8_t *p = out;
	if (cap < 6)
		return 0;
	put_u16(&p, id);
	put_u32(&p, from);
	return (size_t)(p - out);
}

bool asp_start_get(const uint8_t *p, size_t len, uint16_t *id, uint32_t *from)
{
	if (len < 6)
		return false;
	*id = get_u16(p);
	*from = get_u32(p + 2);
	return true;
}

size_t asp_id_put(uint8_t *out, size_t cap, uint16_t id)
{
	uint8_t *p = out;
	if (cap < 2)
		return 0;
	put_u16(&p, id);
	return 2;
}

bool asp_id_get(const uint8_t *p, size_t len, uint16_t *id)
{
	if (len < 2)
		return false;
	*id = get_u16(p);
	return true;
}

size_t asp_text_b_put(uint8_t *out, size_t cap, const asp_text_b *t)
{
	uint8_t *p = out, *end = out + cap;
	if (!put_str(&p, end, t->call, ASP_MAX_CALL - 1))
		return 0;
	if (p + 2 + t->text_len > end)
		return 0;
	put_u16(&p, t->msg_id);
	memcpy(p, t->text, t->text_len);
	p += t->text_len;
	return (size_t)(p - out);
}

bool asp_text_b_get(const uint8_t *p, size_t len, asp_text_b *t)
{
	const uint8_t *end = p + len;
	if (!get_str(&p, end, t->call, sizeof t->call))
		return false;
	if (p + 2 > end)
		return false;
	t->msg_id = get_u16(p);
	p += 2;
	t->text_len = (size_t)(end - p);
	if (t->text_len > sizeof t->text)
		return false;
	memcpy(t->text, p, t->text_len);
	return true;
}

/* --- filenames -------------------------------------------------------------- */

/*
 * The names Windows will not let you create, in any directory, with or without
 * an extension. They are devices, not files, and opening one can block.
 */
static bool reserved_device(const char *name)
{
	static const char *const kReserved[] = {
		"CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5",
		"COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
		"LPT6", "LPT7", "LPT8", "LPT9",
	};

	/* Compared against the stem: "CON.txt" is just as reserved as "CON". */
	size_t stem = 0;
	while (name[stem] && name[stem] != '.')
		stem++;

	for (size_t i = 0; i < sizeof kReserved / sizeof kReserved[0]; i++) {
		const char *r = kReserved[i];
		if (strlen(r) != stem)
			continue;
		size_t j = 0;
		for (; j < stem; j++) {
			char c = name[j];
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 'a' + 'A');
			if (c != r[j])
				break;
		}
		if (j == stem)
			return true;
	}
	return false;
}

bool asp_safe_name(const char *name, char *out, size_t cap)
{
	if (!name || !out || cap < 2)
		return false;

	/*
	 * Take everything after the last separator, and treat both separators on
	 * every platform. A name arriving from a Windows peer carries backslashes
	 * whatever we are running on, and "only handle my own platform's
	 * separator" is how `..\..\` survives on a Unix host.
	 */
	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\' || *p == ':')
			base = p + 1;   /* ':' also drops "C:" */

	size_t n = 0;
	for (const char *p = base; *p && n + 1 < cap; p++) {
		unsigned char c = (unsigned char)*p;

		/* Control characters -- including the newline that would make a
		 * log line lie about what was received -- and the set Windows
		 * refuses. Replaced rather than dropped, so two different
		 * hostile names cannot collapse onto one innocent one. */
		if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '"' ||
		    c == '|' || c == '?' || c == '*')
			out[n++] = '_';
		else
			out[n++] = (char)c;
	}
	out[n] = '\0';

	/* Leading dots and spaces hide a file or make it awkward to remove;
	 * trailing dots and spaces are silently stripped by Windows, which means
	 * the name on disk would differ from the name reported. */
	size_t start = 0;
	while (out[start] == '.' || out[start] == ' ')
		start++;
	if (start)
		memmove(out, out + start, strlen(out + start) + 1);

	n = strlen(out);
	while (n && (out[n - 1] == '.' || out[n - 1] == ' '))
		out[--n] = '\0';

	if (n == 0)
		return false;

	if (reserved_device(out)) {
		if (n + 2 > cap)
			return false;
		out[n] = '_';
		out[n + 1] = '\0';
	}
	return true;
}
