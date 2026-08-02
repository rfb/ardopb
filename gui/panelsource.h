#ifndef ARDOP_GUI_PANELSOURCE_H_
#define ARDOP_GUI_PANELSOURCE_H_

#include <QByteArray>
#include <QObject>
#include <QString>

#include "telemetrytypes.h"

/**
 * @file panelsource.h
 * @brief What the instrument panel needs, from wherever it comes.
 *
 * [analysis/16](../analysis/16-user-interface.md) §8 says the standalone panel
 * survives as a **mode of the same binary**: with `--remote HOST:PORT` the UI
 * attaches to a running `ardopb` instead of an embedded modem, "and everything
 * downstream of the five signals is identical".
 *
 * That was already true in substance -- `SpineSource` was written to emit
 * exactly what `TelemetryClient` emits -- but only by convention, and a window
 * cannot `connect` to a convention. This is the type that makes it structural:
 * two classes, one interface, and the panel binds to the interface.
 *
 * ## Why the five, and only the five
 *
 * These are the signals a *display* needs, and a display is all the remote mode
 * can be. `gui/README.md`: *"Read-only. Nothing here can command the modem --
 * the telemetry stream is one-way by construction, so a display cannot key a
 * transmitter."* Anything that commands the modem therefore belongs on
 * `SpineSource`, not here, and the window shows those screens only when it has
 * an embedded modem to command.
 *
 * The division is not an inconvenience to be worked around later. It is the
 * reason pointing this program at somebody else's station is safe.
 */
class PanelSource : public QObject {
	Q_OBJECT

public:
	explicit PanelSource(QObject *parent = nullptr) : QObject(parent) {}

	/** @brief Spectrum geometry, for the waterfall's frequency axis. */
	virtual int bins() const = 0;
	virtual int firstBin() const = 0;
	virtual float binHz() const = 0;

	/** @brief Whether commanding the modem is possible from this source. */
	virtual bool canCommand() const = 0;

signals:
	void spectrum(const SpectrumRow &row);
	void constellation(const ConstellationFrame &frame);
	void audioLevel(float rms, float peak);
	void status(const LinkStatus &st);
	void connectionChanged(bool up, const QString &detail);
};

#endif /* ARDOP_GUI_PANELSOURCE_H_ */
