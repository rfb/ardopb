#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

extern "C" {
#include "shell/settings.h"
}

#include "modemthread.h"
#include "stationwindow.h"

/**
 * @file main.cpp
 * @brief ardop-station: the modem, with a window on it.
 *
 * One binary, two modes. Embedded is the point of the program; `--remote` keeps
 * the standalone instrument panel alive as a mode rather than a second
 * executable, because watching a station on the other side of the shack is a
 * real capability that embedding does not replace (analysis/16 §5).
 */
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName("ardop-station");

	QCommandLineParser parser;
	parser.setApplicationDescription(
		"The ARDOP station: a modem, its devices, and a window on both.");
	parser.addHelpOption();

	QCommandLineOption telemetryOpt(
		"no-telemetry",
		"Do not compute spectrum and constellation. The panel goes dark; "
		"the modem does not care.");
	parser.addOption(telemetryOpt);

	parser.process(app);

	auto *modem = new ModemThread(&app);
	if (!modem->open(!parser.isSet(telemetryOpt))) {
		QMessageBox::critical(
			nullptr, QObject::tr("ardop station"),
			QObject::tr("Could not start the modem.\n\n"
				    "This is an allocation failure at start-up, "
				    "which usually means the system is out of "
				    "memory."));
		return 1;
	}

	StationWindow window(modem);
	window.show();

	/* The modem thread starts only once the window exists, so nothing it
	 * reports can arrive before there is somewhere to show it. */
	modem->start();

	/*
	 * Open whatever was last used, without being asked.
	 *
	 * A station that has been set up once should start working when it is
	 * started, not wait to be told again -- and if the device has gone the
	 * manager reports a substitution or a failure, which is exactly the
	 * message an operator needs on the screen rather than silence.
	 */
	char path[512];
	if (ardop_settings_path(path, sizeof path)) {
		ardop_settings settings {};
		if (ardop_settings_load(&settings, path)) {
			app_device_selection sel {};
			app_devices_selection_load(&sel, &settings);
			window.applySavedSelection(sel);
		}
	}

	const int rc = app.exec();

	/* Stop the modem before the window goes: teardown unkeys the
	 * transmitter, and a transmitter is not something to leave to the
	 * process exiting. */
	modem->shutdown();
	return rc;
}
