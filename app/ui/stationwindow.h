#ifndef ARDOP_UI_STATIONWINDOW_H_
#define ARDOP_UI_STATIONWINDOW_H_

#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTimer>

#include "constellationwidget.h"
#include "devicespage.h"
#include "gaugewidget.h"
#include "modemthread.h"
#include "spinesource.h"
#include "statuslamps.h"
#include "waterfallwidget.h"

/**
 * @file stationwindow.h
 * @brief The station window: the panel, plus the screens that command the modem.
 *
 * The instrument panel is `gui/`'s, unchanged. What is new is everything that
 * *writes*: this program embeds the modem rather than watching one, so it can
 * choose devices, set the station up and drive a session -- and every one of
 * those crosses the seam by queueing on a ring, never by touching the runtime.
 *
 * `gui/README.md` says of the standalone panel: "Read-only. Nothing here can
 * command the modem -- the telemetry stream is one-way by construction, so a
 * display cannot key a transmitter." That is still true of the *panel widgets*,
 * which is why they could be lifted straight across. It is no longer true of the
 * window around them, and the difference is worth being deliberate about: a
 * window that can key a transmitter has to be one an operator cannot key by
 * accident.
 */
class StationWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit StationWindow(ModemThread *modem, QWidget *parent = nullptr);

	/**
	 * @brief Open @p sel at start-up, and show it in the Devices screen.
	 *
	 * Applied even when it names nothing -- an empty selection means "the
	 * system default", which is the right thing to open on a first run and
	 * is how the program comes up working rather than dark.
	 */
	void applySavedSelection(const app_device_selection &sel);

private slots:
	void onSpectrum(const SpectrumRow &row);
	void onConstellation(const ConstellationFrame &f);
	void onAudio(float rms, float peak);
	void onStatus(const LinkStatus &st);
	void onConnectionChanged(bool up, const QString &detail);
	void onHostMessage(const QString &text);
	void onReply(const QString &text);
	void onFault(int code, const QString &text);
	void onDeviceEvent(int code, const QString &text);
	void onOwnerChanged(bool attached, const QString &text);

private:
	QWidget *buildPanel();
	void log(const QString &line);

	ModemThread *m_modem = nullptr;
	SpineSource m_source;

	QTabWidget *m_tabs = nullptr;
	DevicesPage *m_devices = nullptr;
	QTimer m_slowTick;
	WaterfallWidget *m_waterfall = nullptr;
	ConstellationWidget *m_constellation = nullptr;
	GaugeWidget *m_vu = nullptr;
	GaugeWidget *m_sn = nullptr;
	StatusLamps *m_lamps = nullptr;
	QPlainTextEdit *m_log = nullptr;

	QLabel *m_conn = nullptr;
	QLabel *m_frame = nullptr;
	QLabel *m_owner = nullptr;

	bool m_geometrySet = false;
};

#endif /* ARDOP_UI_STATIONWINDOW_H_ */
