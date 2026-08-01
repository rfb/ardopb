#ifndef ARDOP_GUI_TELEMETRYCLIENT_H_
#define ARDOP_GUI_TELEMETRYCLIENT_H_

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

extern "C" {
#include "shell/telemetry.h"
}

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

/**
 * @brief Reads an ardopb telemetry stream and re-emits it as Qt signals.
 *
 * Reconnects on its own: a display is expected to outlive the daemon it is
 * watching, and to be started before it. Strictly read-only -- nothing this
 * class can do reaches the modem.
 */
class TelemetryClient : public QObject {
	Q_OBJECT

public:
	explicit TelemetryClient(QObject *parent = nullptr);

	/** @brief Connect to @p host : @p port, retrying until told otherwise. */
	void start(const QString &host, quint16 port);

	/** @brief Spectrum geometry from the stream hello, for axis labelling. */
	int bins() const { return m_bins; }
	int firstBin() const { return m_firstBin; }
	float binHz() const { return m_binHz; }
	bool connected() const { return m_greeted; }

signals:
	void spectrum(const SpectrumRow &row);
	void constellation(const ConstellationFrame &frame);
	void audioLevel(float rms, float peak);
	void status(const LinkStatus &st);
	void connectionChanged(bool up, const QString &detail);

private slots:
	void onReadyRead();
	void onConnected();
	void onDisconnected();
	void retry();

private:
	void consume();

	QTcpSocket m_sock;
	QTimer m_retry;
	QByteArray m_buf;
	QString m_host;
	quint16 m_port = 0;

	bool m_greeted = false;
	int m_bins = ARDOP_BUSY_MAG_BINS;
	int m_firstBin = ARDOP_BUSY_FIRST_BIN;
	float m_binHz = ARDOP_BUSY_BIN_HZ;

	/* Decoding into a member rather than a local: the decoded record owns
	 * several kB of arrays, and this runs once per spectrum row. */
	ardop_tlm_decoded m_decoded {};
};

#endif /* ARDOP_GUI_TELEMETRYCLIENT_H_ */
