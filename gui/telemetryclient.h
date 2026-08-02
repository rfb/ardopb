#ifndef ARDOP_GUI_TELEMETRYCLIENT_H_
#define ARDOP_GUI_TELEMETRYCLIENT_H_

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include "panelsource.h"
#include "telemetrytypes.h"

extern "C" {
#include "shell/telemetry.h"
}

/**
 * @brief Reads an ardopb telemetry stream and re-emits it as Qt signals.
 *
 * Reconnects on its own: a display is expected to outlive the daemon it is
 * watching, and to be started before it. Strictly read-only -- nothing this
 * class can do reaches the modem.
 */
class TelemetryClient : public PanelSource {
	Q_OBJECT

public:
	explicit TelemetryClient(QObject *parent = nullptr);

	/** @brief Never. A telemetry stream is one-way; see panelsource.h. */
	bool canCommand() const override { return false; }

	/** @brief Connect to @p host : @p port, retrying until told otherwise. */
	void start(const QString &host, quint16 port);

	/** @brief Spectrum geometry from the stream hello, for axis labelling. */
	int bins() const override { return m_bins; }
	int firstBin() const override { return m_firstBin; }
	float binHz() const override { return m_binHz; }
	bool connected() const { return m_greeted; }

	/* The five signals are declared by PanelSource and emitted from here. */

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
