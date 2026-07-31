#include <QApplication>
#include <QCommandLineParser>
#include <QStringList>

#include "mainwindow.h"

/**
 * @file main.cpp
 * @brief ardop-gui -- the instrument panel for a running ardopb.
 *
 * Start the modem with a telemetry port, then point this at it:
 *
 *   ardopb N0CALL --alsa default default --host 8515 --telemetry
 *   ardop-gui --host 127.0.0.1:8517
 *
 * The default port is 8517, which is what --telemetry picks when --host is
 * 8515. Nothing here can command the modem: the stream is one-way, so this can
 * be pointed at a remote station's TNC without giving anyone a transmitter.
 */

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("ardop-gui"));

	QCommandLineParser parser;
	parser.setApplicationDescription(
		QStringLiteral("Instrument panel for an ardopb telemetry stream."));
	parser.addHelpOption();

	QCommandLineOption hostOpt(
		QStringList() << QStringLiteral("host"),
		QStringLiteral("Telemetry endpoint as HOST:PORT, or just PORT."),
		QStringLiteral("endpoint"), QStringLiteral("127.0.0.1:8517"));
	parser.addOption(hostOpt);
	parser.process(app);

	QString host = QStringLiteral("127.0.0.1");
	quint16 port = 8517;

	const QString endpoint = parser.value(hostOpt);
	const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
	if (colon >= 0) {
		host = endpoint.left(colon);
		port = quint16(endpoint.mid(colon + 1).toUInt());
	} else {
		bool numeric = false;
		const uint n = endpoint.toUInt(&numeric);
		if (numeric)
			port = quint16(n);
		else
			host = endpoint;
	}
	if (host.isEmpty())
		host = QStringLiteral("127.0.0.1");

	MainWindow w(host, port);
	w.show();
	return app.exec();
}
