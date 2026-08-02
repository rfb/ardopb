#ifndef ARDOP_GUI_TELEMETRYTYPES_H_
#define ARDOP_GUI_TELEMETRYTYPES_H_

#include <QVector>
#include <QtGlobal>

/**
 * @file telemetrytypes.h
 * @brief The three payloads the instruments are drawn from.
 *
 * Split out of telemetryclient.h so that panelsource.h -- and therefore anything
 * that only *displays* -- can name them without pulling in a TCP socket. The
 * embedded source produces these from a queue and the remote one from a stream;
 * neither difference is visible here, which is the point.
 */

/**
 * @brief One spectrum row, as the waterfall wants it.
 *
 * Power magnitudes straight from the modem's FFT. Scaling to dB and to colour
 * is the widget's business, not the transport's.
 */
struct SpectrumRow {
	QVector<float> mag;
};

/** @brief One frame's demodulated symbols, in the decoder's own units. */
struct ConstellationFrame {
	quint8 frameType = 0;
	quint8 modulation = 0;
	/* Interpretation depends on `modulation` -- see shell/telemetry.h.
	 * PSK/QAM: phase in milliradians and symbol magnitude.
	 * 4FSK:    winning tone 0..3 and decision margin in per mille. */
	QVector<qint16> phaseMrad;
	QVector<qint16> mag;
	qint16 magThreshold = 0;     /**< 16QAM ring, in `mag` units; 0 if none. */
};

/** @brief The discrete state a panel needs, mirrored onto the stream. */
struct LinkStatus {
	quint8 state = 0;
	quint8 mode = 0;
	bool busy = false;
	bool ptt = false;
	qint16 sn = 0;
	qint16 quality = 0;
	qint16 bandwidth = 0;
	quint32 bufferLen = 0;
};

#endif /* ARDOP_GUI_TELEMETRYTYPES_H_ */
