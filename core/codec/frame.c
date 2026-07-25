#include <stddef.h>
#include <string.h>

#include "codec/frame.h"

/*
 * The frame type table.
 *
 * Normative protocol data, transcribed mechanically from `FrameInfo()` in
 * src/common/ARDOPC.c rather than by hand, and pinned by
 * test/core/test_frame.c, which compares all 256 type bytes against that
 * function. Changing a value here changes what goes on the air.
 *
 * Columns, in order:
 *     name, modulation, baud, carriers, data bytes, RS bytes, quality
 *
 * Data and RS byte counts are *per carrier*, not per frame.
 *
 * Positional rather than designated initialisers, because a 57-row table of
 * seven scalars reads and diffs better as columns. That is safe here only
 * because reordering the struct fields would change at least one value for at
 * least one of the 256 types, and the equivalence test checks all of them.
 *
 * Frame types 0x00-0x1F (DataNAK) and 0xE0-0xFF (DataACK) are absent by
 * design: each of those 32-byte ranges shares one specification, and the low
 * five bits carry a quality value rather than selecting a different frame
 * shape. They are handled in ardop_frame_spec_for().
 *
 * The 135 type bytes with no entry are not valid ARDOP frame types.
 */
static const ardop_frame_spec frame_table[256] = {

	/* Short control frames: no payload at all, and a high quality bar --
	   a false positive here costs more than a retransmission. */
	[0x23] = { "BREAK",             ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },
	[0x24] = { "IDLE",              ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },
	[0x29] = { "DISC",              ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },
	[0x2C] = { "END",               ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },
	[0x2D] = { "ConRejBusy",        ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },
	[0x2E] = { "ConRejBW",          ARDOP_MOD_4FSK,    50, 1,   0,   0, 60 },

	/* Control frames carrying a small payload: callsigns, grid squares,
	   leader length, signal reports. */
	[0x30] = { "IDFrame",           ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x31] = { "ConReq200M",        ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x32] = { "ConReq500M",        ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x33] = { "ConReq1000M",       ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x34] = { "ConReq2000M",       ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x35] = { "ConReq200F",        ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x36] = { "ConReq500F",        ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x37] = { "ConReq1000F",       ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x38] = { "ConReq2000F",       ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },
	[0x39] = { "ConAck200",         ARDOP_MOD_4FSK,    50, 1,   3,   0, 50 },
	[0x3A] = { "ConAck500",         ARDOP_MOD_4FSK,    50, 1,   3,   0, 50 },
	[0x3B] = { "ConAck1000",        ARDOP_MOD_4FSK,    50, 1,   3,   0, 50 },
	[0x3C] = { "ConAck2000",        ARDOP_MOD_4FSK,    50, 1,   3,   0, 50 },
	[0x3D] = { "PingAck",           ARDOP_MOD_4FSK,    50, 1,   3,   0, 50 },
	[0x3E] = { "Ping",              ARDOP_MOD_4FSK,    50, 1,  12,   4, 50 },

	/* 200 Hz bandwidth data frames, single carrier. */
	[0x40] = { "4PSK.200.100.E",    ARDOP_MOD_4PSK,   100, 1,  64,  32, 30 },
	[0x41] = { "4PSK.200.100.O",    ARDOP_MOD_4PSK,   100, 1,  64,  32, 30 },
	[0x42] = { "4PSK.200.100S.E",   ARDOP_MOD_4PSK,   100, 1,  16,   8, 30 },
	[0x43] = { "4PSK.200.100S.O",   ARDOP_MOD_4PSK,   100, 1,  16,   8, 30 },
	[0x44] = { "8PSK.200.100.E",    ARDOP_MOD_8PSK,   100, 1, 108,  36, 30 },
	[0x45] = { "8PSK.200.100.O",    ARDOP_MOD_8PSK,   100, 1, 108,  36, 30 },
	[0x46] = { "16QAM.200.100.E",   ARDOP_MOD_16QAM,  100, 1, 128,  64, 30 },
	[0x47] = { "16QAM.200.100.O",   ARDOP_MOD_16QAM,  100, 1, 128,  64, 30 },
	[0x48] = { "4FSK.200.50S.E",    ARDOP_MOD_4FSK,    50, 1,  16,   4, 30 },
	[0x49] = { "4FSK.200.50S.O",    ARDOP_MOD_4FSK,    50, 1,  16,   4, 30 },
	[0x4A] = { "4FSK.500.100.E",    ARDOP_MOD_4FSK,   100, 1,  64,  16, 30 },
	[0x4B] = { "4FSK.500.100.O",    ARDOP_MOD_4FSK,   100, 1,  64,  16, 30 },
	[0x4C] = { "4FSK.500.100S.E",   ARDOP_MOD_4FSK,   100, 1,  32,   8, 30 },
	[0x4D] = { "4FSK.500.100S.O",   ARDOP_MOD_4FSK,   100, 1,  32,   8, 30 },

	/* 500 Hz bandwidth data frames. */
	[0x50] = { "4PSK.500.100.E",    ARDOP_MOD_4PSK,   100, 2,  64,  32, 50 },
	[0x51] = { "4PSK.500.100.O",    ARDOP_MOD_4PSK,   100, 2,  64,  32, 50 },
	[0x52] = { "8PSK.500.100.E",    ARDOP_MOD_8PSK,   100, 2, 108,  36, 50 },
	[0x53] = { "8PSK.500.100.O",    ARDOP_MOD_8PSK,   100, 2, 108,  36, 50 },
	[0x54] = { "16QAM.500.100.E",   ARDOP_MOD_16QAM,  100, 2, 128,  64, 50 },
	[0x55] = { "16QAM.500.100.O",   ARDOP_MOD_16QAM,  100, 2, 128,  64, 50 },

	/* 1000 Hz bandwidth data frames. */
	[0x60] = { "4PSK.1000.100.E",   ARDOP_MOD_4PSK,   100, 4,  64,  32, 50 },
	[0x61] = { "4PSK.1000.100.O",   ARDOP_MOD_4PSK,   100, 4,  64,  32, 50 },
	[0x62] = { "8PSK.1000.100.E",   ARDOP_MOD_8PSK,   100, 4, 108,  36, 50 },
	[0x63] = { "8PSK.1000.100.O",   ARDOP_MOD_8PSK,   100, 4, 108,  36, 50 },
	[0x64] = { "16QAM.1000.100.E",  ARDOP_MOD_16QAM,  100, 4, 128,  64, 50 },
	[0x65] = { "16QAM.1000.100.O",  ARDOP_MOD_16QAM,  100, 4, 128,  64, 50 },

	/* 2000 Hz bandwidth data frames, eight carriers. */
	[0x70] = { "4PSK.2000.100.E",   ARDOP_MOD_4PSK,   100, 8,  64,  32, 50 },
	[0x71] = { "4PSK.2000.100.O",   ARDOP_MOD_4PSK,   100, 8,  64,  32, 50 },
	[0x72] = { "8PSK.2000.100.E",   ARDOP_MOD_8PSK,   100, 8, 108,  36, 50 },
	[0x73] = { "8PSK.2000.100.O",   ARDOP_MOD_8PSK,   100, 8, 108,  36, 50 },
	[0x74] = { "16QAM.2000.100.E",  ARDOP_MOD_16QAM,  100, 8, 128,  64, 50 },
	[0x75] = { "16QAM.2000.100.O",  ARDOP_MOD_16QAM,  100, 8, 128,  64, 50 },

	/* 2000 Hz bandwidth at 600 baud. Intended for FM, not SSB. */
	[0x7A] = { "4FSK.2000.600.E",   ARDOP_MOD_4FSK,   600, 1, 600, 150, 30 },
	[0x7B] = { "4FSK.2000.600.O",   ARDOP_MOD_4FSK,   600, 1, 600, 150, 30 },
	[0x7C] = { "4FSK.2000.600S.E",  ARDOP_MOD_4FSK,   600, 1, 200,  50, 30 },
	[0x7D] = { "4FSK.2000.600S.O",  ARDOP_MOD_4FSK,   600, 1, 200,  50, 30 },
};

/*
 * 0x00-0x1F. The low five bits encode the reported decode quality, so all 32
 * bytes describe the same frame shape.
 */
static const ardop_frame_spec data_nak_spec = {
	"DataNAK", ARDOP_MOD_4FSK, 50, 1, 0, 0, 40
};

/* 0xE0-0xFF, on the same principle as DataNAK. */
static const ardop_frame_spec data_ack_spec = {
	"DataACK", ARDOP_MOD_4FSK, 50, 1, 0, 0, 40
};

const ardop_frame_spec *ardop_frame_spec_for(uint8_t frame_type)
{
	if (frame_type <= ARDOP_FRAME_DATA_NAK_MAX) {
		return &data_nak_spec;
	}
	if (frame_type >= ARDOP_FRAME_DATA_ACK_MIN) {
		return &data_ack_spec;
	}

	/* frame_type is a uint8_t, so it cannot index outside the table. */
	const ardop_frame_spec *spec = &frame_table[frame_type];
	return spec->name != NULL ? spec : NULL;
}

bool ardop_frame_is_data(uint8_t frame_type)
{
	const ardop_frame_spec *spec = ardop_frame_spec_for(frame_type);

	/* A data frame is one whose name carries the even/odd suffix, matching
	 * the inherited IsDataFrame (which tests the name for ".E"/".O"). */
	if (spec == NULL || spec->name == NULL || spec->name[0] == 0)
		return false;
	return strstr(spec->name, ".E") != NULL
	       || strstr(spec->name, ".O") != NULL;
}

uint8_t ardop_frame_type_parity(uint8_t frame_type)
{
	uint8_t mask = 0xC0;
	uint8_t parity_sum = 1;

	for (int k = 0; k < 4; k++) {
		uint8_t sym = (uint8_t)((mask & frame_type) >> (2 * (3 - k)));
		parity_sum = (uint8_t)(parity_sum ^ sym);
		mask = (uint8_t)(mask >> 2);
	}

	return (uint8_t)(parity_sum & 0x3u);
}

const char *ardop_modulation_name(ardop_modulation modulation)
{
	switch (modulation) {
	case ARDOP_MOD_4FSK:
		return "4FSK";
	case ARDOP_MOD_4PSK:
		return "4PSK";
	case ARDOP_MOD_8PSK:
		return "8PSK";
	case ARDOP_MOD_16QAM:
		return "16QAM";
	}
	/*
	 * Unreachable for any valid enumerator. Deliberately outside the
	 * switch, so that -Wswitch reports a new enumerator as a build error
	 * rather than it falling through to this line unnoticed.
	 */
	return "?";
}
