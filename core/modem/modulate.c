#include "modem/modulate.h"

#include <math.h>
#include <string.h>

#include "codec/frame.h"
#include "modem/templates.h"

/*
 * The transmit DSP, transcribed from the inherited Mod4FSKDataAndPlay /
 * SendLeaderAndSYNC / SampleSink / initFilter / AddTrailer, with the I/O removed
 * (no SendtoCard, no PTT, no wall-clock wait) and the file-scope filter state
 * moved into ardop_mod. The maths is unchanged and normative; the golden vectors
 * pin every output sample.
 *
 * Only 4FSK at 50 and 100 baud is implemented so far -- enough for the control
 * frames and the first golden check. 600-baud 4FSK and PSK/QAM follow.
 */

enum {
	FILTER_LEN = 120,          /* intN: 12000/100 */
	LEADER_MS_DEFAULT = 240,   /* inherited LeaderLength default */
	TRAILER_MS_DEFAULT = 20,   /* inherited TrailerLength default */
};

/* 0.9995 keeps the resonators stable (< 1.0); the value the reference uses. */
static const float FILTER_R = 0.9995f;

/*
 * NOT the mathematical pi. The inherited headers (ARDOPC.h) redefine M_PI as
 * this reduced-precision *float* literal, and the resonator coefficients -- and
 * therefore the exact samples that go on the air -- are computed with it. Using
 * the true double pi shifts the coefficients by ~1 ULP and desynchronises the
 * output from every other ARDOP station. This truncated value is normative by
 * accident, and must be preserved bit-for-bit.
 */
static const float ARDOP_PI = 3.1415926f;

/* The 5th frame-type sync symbol: parity over the four 2-bit symbols of the
 * type byte. Ported from ComputeTypeParity() in ARDOPC.c. */
static uint8_t type_parity(uint8_t frame_type)
{
	uint8_t mask = 0xC0;
	uint8_t parity = 1;

	for (int k = 0; k < 4; k++) {
		uint8_t sym = (uint8_t)((mask & frame_type) >> (2 * (3 - k)));
		parity ^= sym;
		mask = (uint8_t)(mask >> 2);
	}
	return parity & 0x3u;
}

/*
 * Feed one input sample through the comb + resonator filter. After a warm-up of
 * FILTER_LEN/2 samples each input yields one filtered output sample, appended to
 * the caller's frame buffer. Ported from SampleSink(); the debug peak-tracking
 * (largest/smallest) is dropped as it never affected the output.
 */
static void sample_sink(ardop_mod *m, short sample)
{
	const int fil_len = FILTER_LEN / 2;
	float filtered = 0;

	sample = (short)(sample * m->drive_level / 100);

	if (m->sample_no < FILTER_LEN)
		m->zin = sample;
	else
		m->zin = sample - m->rn * m->last120[m->last120_get];

	if (++m->last120_get == 121)
		m->last120_get = 0;

	m->zcomb = m->zin - m->zin_2 * m->r2;
	m->zin_2 = m->zin_1;
	m->zin_1 = m->zin;

	for (int j = m->first; j <= m->last; j++) {
		m->zout_0[j] = m->zcomb + m->coef[j] * m->zout_1[j]
			     - m->r2 * m->zout_2[j];
		m->zout_2[j] = m->zout_1[j];
		m->zout_1[j] = m->zout_0[j];

		if (m->sample_no < fil_len)
			continue;

		switch (m->fwidth) {
		case 200:
			if (j == m->first || j == m->last)
				filtered += 0.7389f * m->zout_0[j];
			else
				filtered -= m->zout_0[j];
			break;
		case 500:
			if (j == m->first || j == m->last)
				filtered += 0.10601f * m->zout_0[j];
			else if (j == m->first + 1 || j == m->last - 1)
				filtered -= 0.59383f * m->zout_0[j];
			else if ((j & 1) == 0)
				filtered += (float)(int)m->zout_0[j];
			else
				filtered -= (float)(int)m->zout_0[j];
			break;
		case 1000:
			if (j == m->first || j == m->last)
				filtered += 0.377f * m->zout_0[j];
			else if ((j & 1) == 0)
				filtered += (float)(int)m->zout_0[j];
			else
				filtered -= (float)(int)m->zout_0[j];
			break;
		case 2000:
			if (j == m->first || j == m->last)
				filtered += 0.371f * m->zout_0[j];
			else if ((j & 1) == 0)
				filtered += (float)(int)m->zout_0[j];
			else
				filtered -= (float)(int)m->zout_0[j];
			break;
		default:
			break;
		}
	}

	if (m->sample_no >= fil_len) {
		filtered = filtered * 0.00833333333f;  /* undo filter gain */
		if (filtered > 32700)
			filtered = 32700;
		else if (filtered < -32700)
			filtered = -32700;

		if (m->frame_len < m->frame_cap)
			m->frame[m->frame_len++] = (int16_t)(short)filtered;
		else
			m->overflow = true;
	}

	m->last120[m->last120_put++] = sample;
	if (m->last120_put == 121)
		m->last120_put = 0;

	m->sample_no++;
}

/* Configure the filter for a frame. Ported from initFilter(), I/O removed. */
static void init_filter(ardop_mod *m, int width, int centre)
{
	int centre_slot = centre / 100;

	m->fwidth = width;
	m->sample_no = 0;
	memset(m->last120, 0, sizeof(m->last120));
	m->last120_get = 0;
	m->last120_put = 120;

	m->rn = powf(FILTER_R, FILTER_LEN);
	m->r2 = powf(FILTER_R, 2);
	m->zin_1 = 0;
	m->zin_2 = 0;

	switch (width) {
	case 200:
		m->first = centre_slot - 1;
		m->last = centre_slot + 1;
		break;
	case 500:
		m->first = centre_slot - 3;
		m->last = centre_slot + 3;
		break;
	case 1000:
		m->first = centre_slot - 5;
		m->last = centre_slot + 5;
		break;
	case 2000:
		m->first = centre_slot - 10;
		m->last = centre_slot + 10;
		break;
	default:
		m->first = centre_slot;
		m->last = centre_slot;
		break;
	}

	for (int j = m->first; j <= m->last; j++) {
		m->zout_0[j] = 0;
		m->zout_1[j] = 0;
		m->zout_2[j] = 0;
	}

	/* The reference caches these across frames; recomputing every frame is
	 * identical because cosf is deterministic. */
	for (int i = m->first; i <= m->last; i++)
		m->coef[i] = 2 * FILTER_R
			   * cosf(2 * ARDOP_PI * (float)i / FILTER_LEN);
}

/* The two-tone leader: sym_len symbols of the 50-baud leader template, with the
 * last symbol inverted. Ported from GetTwoToneLeaderWithSync(). */
static void two_tone_leader(ardop_mod *m, int sym_len)
{
	int sign = ((sym_len & 1) == 1) ? -1 : 1;

	for (int i = 0; i < sym_len; i++) {
		for (int j = 0; j < 240; j++) {
			short s = int50BaudTwoToneLeaderTemplate[j];
			if (i != sym_len - 1)
				s = (short)(sign * s);
			else
				s = (short)(-sign * s);
			sample_sink(m, s);
		}
		sign = -sign;
	}
}

/* Leader plus the two frame-type sync bytes as 50-baud 4FSK. Ported from
 * SendLeaderAndSYNC(). */
static void send_leader_and_sync(ardop_mod *m, const uint8_t *encoded,
				 int leader_ms)
{
	int leader_len_ms = (leader_ms == 0) ? LEADER_MS_DEFAULT : leader_ms;

	two_tone_leader(m, leader_len_ms / 20);

	for (int j = 0; j < 2; j++) {
		uint8_t mask = 0xC0;
		for (int k = 0; k < 5; k++) {
			uint8_t sym;
			if (k < 4)
				sym = (uint8_t)((mask & encoded[j])
						>> (2 * (3 - k)));
			else
				sym = type_parity(encoded[0]);

			for (int n = 0; n < 240; n++) {
				short t = intFSK50bdCarTemplate[sym][n];
				short s = (((5 * j + k) & 1) == 0)
					? t : (short)(-t);
				sample_sink(m, s);
			}
			mask = (uint8_t)(mask >> 2);
		}
	}
}

/* One carrier of 4FSK data at 50 or 100 baud. Ported from the data loop of
 * Mod4FSKDataAndPlay(). */
static void modulate_4fsk_data(ardop_mod *m, const uint8_t *encoded, size_t len,
			       uint16_t baud, uint8_t carriers)
{
	int data_bytes_per_car = (int)((len - 2) / carriers);
	int samp_per_sym = (baud == 50) ? 240 : 120;
	int data_ptr = 2;

	for (int b = 0; b < data_bytes_per_car; b++) {
		uint8_t mask = 0xC0;
		for (int k = 0; k < 4; k++) {
			uint8_t sym = (uint8_t)((mask & encoded[data_ptr])
						>> (2 * (3 - k)));
			for (int n = 0; n < samp_per_sym; n++) {
				short t = (baud == 50)
					? intFSK50bdCarTemplate[sym][n]
					: intFSK100bdCarTemplate[sym][n];
				short s = ((k & 1) == 0) ? t : (short)(-t);
				sample_sink(m, s);
			}
			mask = (uint8_t)(mask >> 2);
		}
		data_ptr++;
	}
}

/* 600-baud 4FSK: like the 50/100 path but width 2000, 20 samples/symbol, its
 * own template, and no per-symbol sign inversion. Ported from Mod4FSK600Bd. */
static void modulate_4fsk600_data(ardop_mod *m, const uint8_t *encoded,
				  size_t len, uint8_t carriers)
{
	int data_bytes_per_car = (int)((len - 2) / carriers);
	int samp_per_sym = ARDOP_MOD_SAMPLE_RATE / 600;  /* 20 */
	int data_ptr = 2;

	for (int b = 0; b < data_bytes_per_car; b++) {
		uint8_t mask = 0xC0;
		for (int k = 0; k < 4; k++) {
			uint8_t sym = (uint8_t)((mask & encoded[data_ptr])
						>> (2 * (3 - k)));
			for (int n = 0; n < samp_per_sym; n++)
				sample_sink(m, intFSK600bdCarTemplate[sym][n]);
			mask = (uint8_t)(mask >> 2);
		}
		data_ptr++;
	}
}

/* Soft-clip a summed PSK/QAM sample beyond +/-30000 with a sqrt taper. Ported
 * from SoftClip; the clip-count diagnostic it kept is dropped (I/O-free core). */
static int soft_clip(int input)
{
	if (input > 30000) {
		double v = 30000 + 20 * sqrt((double)(input - 30000));
		return (int)(v < 32700 ? v : 32700);
	}
	if (input < -30000) {
		double v = -30000 - 20 * sqrt((double)(-(input + 30000)));
		return (int)(v > -32700 ? v : -32700);
	}
	return input;
}

/*
 * Phase/magnitude symbols for one carrier from its bytes. The low 3 bits are a
 * differential phase (0..7 = 0..315 deg); bit 3 flags half magnitude. 4PSK
 * steps the phase by 2 so it uses only even phases. Ported from
 * Calc1CarPSKSymbols. Returns the number of symbols written.
 */
static int calc_psk_symbols(uint8_t *symbols, ardop_modulation mod,
			    const uint8_t *src, int src_len)
{
	uint16_t databuf = 0;
	int bits_buffered = 0;
	int bits_per_symbol;
	int sym_set = 1;

	switch (mod) {
	case ARDOP_MOD_4PSK: sym_set = 2; bits_per_symbol = 2; break;
	case ARDOP_MOD_8PSK: bits_per_symbol = 3; break;
	case ARDOP_MOD_16QAM: bits_per_symbol = 4; break;
	default: return 0;
	}

	int symbol_count = src_len * 8 / bits_per_symbol;
	for (int i = 0; i < symbol_count; i++) {
		if (bits_buffered < bits_per_symbol) {
			databuf = (uint16_t)(databuf
				+ ((unsigned)*src++ << (8 - bits_buffered)));
			bits_buffered += 8;
		}
		uint8_t raw = (uint8_t)(databuf >> (16 - bits_per_symbol));
		databuf = (uint16_t)(databuf << bits_per_symbol);
		bits_buffered -= bits_per_symbol;

		if (i == 0)
			symbols[i] = (uint8_t)((raw * sym_set) & 7);
		else
			symbols[i] = (uint8_t)((symbols[i - 1]
					       + raw * sym_set) & 7);
		symbols[i] = (uint8_t)(symbols[i] + (raw & 0x08));
	}
	return symbol_count;
}

/*
 * Sum every carrier's template at each sample, scale, soft-clip, emit. Phases
 * 4..7 are the negatives of 0..3; bit 3 halves the magnitude via a right shift.
 * Carrier 4 (1500 Hz) is the single-carrier slot and is skipped by multi-carrier
 * modes. Ported from PlayPSKSymbols.
 */
static void play_psk_symbols(ardop_mod *m, uint8_t symbols[8][462],
			     int num_cars, int symbol_count, float scaling)
{
	const int samp_per_sym = 120;
	int car_start = (num_cars == 1) ? 4 : (num_cars == 2) ? 3
		      : (num_cars == 4) ? 2 : 0;

	for (int sy = 0; sy < symbol_count; sy++) {
		for (int n = 0; n < samp_per_sym; n++) {
			int sample = 0;
			int car_index = car_start;
			for (int i = 0; i < num_cars; i++) {
				uint8_t sym = symbols[i][sy];
				int phase = sym & 0x07;
				int shift = sym >> 3;
				if (phase < 4)
					sample += intPSK100bdCarTemplate
						[car_index][phase][n] >> shift;
				else
					sample -= intPSK100bdCarTemplate
						[car_index][phase - 4][n] >> shift;
				car_index++;
				if (car_index == 4)
					car_index++;  /* skip 1500 Hz slot */
			}
			/* The reference scales in double: its scaling factor is
			 * a float literal passed through a double parameter, so
			 * the multiply happens in double precision. Doing it in
			 * float drifts by 1 LSB on some carrier counts. */
			sample = (int)((double)sample * (double)scaling);
			sample_sink(m, (short)soft_clip(sample));
		}
	}
}

/* Empirical per-carrier scaling factors (crest-factor tuned). From ModPSK. */
static float psk_scaling(uint8_t carriers, ardop_modulation mod)
{
	bool qam = (mod == ARDOP_MOD_16QAM);

	switch (carriers) {
	case 1: return 1.2f;
	case 2: return qam ? 0.67f : 0.65f;
	case 4: return 0.4f;
	case 8: return qam ? 0.27f : 0.25f;
	default: return 1.0f;
	}
}

/*
 * 4PSK / 8PSK / 16QAM over 1..8 carriers. A leading reference symbol (phase 0,
 * full magnitude) sets the phase datum the differential data symbols are
 * measured against. Ported from ModPSKDataAndPlay. Returns false if `encoded`
 * is too short for the frame the spec describes.
 */
static bool modulate_psk(ardop_mod *m, const ardop_frame_spec *spec,
			 const uint8_t *encoded, size_t len, uint16_t leader_ms)
{
	int num_car = spec->carriers;
	int bytes_per_carrier = spec->data_bytes_per_carrier
			      + spec->rs_bytes_per_carrier + 3;

	if (2 + (size_t)num_car * (size_t)bytes_per_carrier > len)
		return false;

	int width = (num_car == 1) ? 200 : (num_car == 2) ? 500
		  : (num_car == 4) ? 1000 : 2000;
	init_filter(m, width, 1500);
	send_leader_and_sync(m, encoded, leader_ms);

	float scaling = psk_scaling(spec->carriers, spec->modulation);
	uint8_t symbols[8][462] = {{0}};

	/* One reference symbol on every carrier, before the data. */
	for (int car = 0; car < num_car; car++)
		symbols[car][0] = 0;
	play_psk_symbols(m, symbols, num_car, 1, scaling);

	int data_ptr = 2;
	int symbol_count = 0;
	for (int car = 0; car < num_car; car++) {
		symbol_count = calc_psk_symbols(symbols[car], spec->modulation,
						&encoded[data_ptr],
						bytes_per_carrier);
		data_ptr += bytes_per_carrier;
	}
	play_psk_symbols(m, symbols, num_car, symbol_count, scaling);
	return true;
}

/* The trailer: 1 symbol plus one per 10 ms of trailer, of a fixed PSK template.
 * Ported from AddTrailer() (invoked by SoundFlush()). */
static void add_trailer(ardop_mod *m, int trailer_ms)
{
	int added = 1 + (trailer_ms / 10);

	for (int i = 1; i <= added; i++)
		for (int k = 0; k < 120; k++)
			sample_sink(m, intPSK100bdCarTemplate[4][0][k]);
}

void ardop_mod_init(ardop_mod *m, uint8_t drive_level)
{
	memset(m, 0, sizeof(*m));
	m->drive_level = drive_level;
}

bool ardop_mod_begin(ardop_mod *m, uint8_t frame_type, const uint8_t *encoded,
		     size_t len, uint16_t leader_ms, int16_t *sample_buf,
		     size_t sample_cap)
{
	const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);
	if (spec == NULL || len < 2)
		return false;

	m->frame = sample_buf;
	m->frame_cap = sample_cap;
	m->frame_len = 0;
	m->frame_pos = 0;
	m->overflow = false;

	/* Each modulation sets up its own filter, then lays down leader and data;
	 * the trailer is common. The filter bandwidth is chosen by baud for 4FSK
	 * and by carrier count for PSK. */
	switch (spec->modulation) {
	case ARDOP_MOD_4FSK:
		if (spec->baud == 600) {
			init_filter(m, 2000, 1500);
			send_leader_and_sync(m, encoded, leader_ms);
			modulate_4fsk600_data(m, encoded, len, spec->carriers);
		} else {
			init_filter(m, spec->baud == 50 ? 200 : 500, 1500);
			send_leader_and_sync(m, encoded, leader_ms);
			modulate_4fsk_data(m, encoded, len, spec->baud,
					   spec->carriers);
		}
		break;
	case ARDOP_MOD_4PSK:
	case ARDOP_MOD_8PSK:
	case ARDOP_MOD_16QAM:
		if (!modulate_psk(m, spec, encoded, len, leader_ms))
			return false;
		break;
	default:
		return false;
	}

	add_trailer(m, TRAILER_MS_DEFAULT);
	return !m->overflow;
}

size_t ardop_mod_pull(ardop_mod *m, int16_t *out, size_t max)
{
	size_t avail = m->frame_len - m->frame_pos;
	size_t n = (avail < max) ? avail : max;

	memcpy(out, &m->frame[m->frame_pos], n * sizeof(*out));
	m->frame_pos += n;
	return n;
}

bool ardop_mod_busy(const ardop_mod *m)
{
	return m->frame_pos < m->frame_len;
}
