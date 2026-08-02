#ifndef ARDOP_UI_STATIONWINDOW_H_
#define ARDOP_UI_STATIONWINDOW_H_

#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTimer>

#include "aspsession.h"
#include "chatpage.h"
#include "constellationwidget.h"
#include "consolepage.h"
#include "devicespage.h"
#include "filespage.h"
#include "guestspage.h"
#include "historypage.h"
#include "levelmeter.h"
#include "modemthread.h"
#include "sessionhistory.h"
#include "spinesource.h"
#include "stationpage.h"
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
	/**
	 * @brief The embedded station: a modem, its devices, and every screen.
	 */
	explicit StationWindow(ModemThread *modem, QWidget *parent = nullptr);

	/**
	 * @brief The remote panel: watch a running `ardopb` over its telemetry
	 *        port.
	 *
	 * analysis/16 §8. **Only the Panel appears** -- there is no Devices,
	 * Station or Console screen, because there is nothing to command. That
	 * is not a limitation to be lifted later; `gui/README.md` records it as
	 * the property that makes pointing this program at somebody else's
	 * station safe: "the telemetry stream is one-way by construction, so a
	 * display cannot key a transmitter".
	 *
	 * The window takes ownership of the client it builds.
	 */
	StationWindow(const QString &host, quint16 port,
		      QWidget *parent = nullptr);

	/**
	 * @brief Open @p sel at start-up, and show it in the Devices screen.
	 *
	 * Applied even when it names nothing -- an empty selection means "the
	 * system default", which is the right thing to open on a first run and
	 * is how the program comes up working rather than dark.
	 */
	void applySavedSelection(const app_device_selection &sel);

	/**
	 * @brief Load the saved station identity and push it to the modem.
	 *
	 * Separate from applySavedSelection because they answer different
	 * questions and can fail independently: a station with no sound card is
	 * still a station with a callsign, and an operator who has lost their
	 * device selection should not also have to retype their grid.
	 */
	void applySavedStation(const ardop_settings *s);

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
	void onLinkState(int state, const QString &remote);
	void onGuestEvent(int code, const QString &text);

private slots:
	/** @brief Write `station.*` out. Debounced -- see the implementation. */
	void saveStation();

protected:
	/** @brief Flush a debounced save, so quitting cannot discard one. */
	void closeEvent(QCloseEvent *event) override;

private:
	QWidget *buildPanel();
	void connectPanel();
	void log(const QString &line);

	/* Null in remote mode. Every use is guarded, and the guard is what
	 * decides whether a screen exists at all. */
	ModemThread *m_modem = nullptr;

	/* Exactly one of these is built; m_source is the one the panel binds
	 * to. See panelsource.h. */
	SpineSource *m_spineSource = nullptr;
	TelemetryClient *m_client = nullptr;
	PanelSource *m_source = nullptr;

	/* The chat and file protocol. One session, two screens: they travel over
	 * the same connection and share one transmit credit, so a second owner
	 * would be a second answer to "are we connected". */
	AspSession *m_asp = nullptr;

	QTabWidget *m_tabs = nullptr;
	DevicesPage *m_devices = nullptr;
	StationPage *m_station = nullptr;
	ChatPage *m_chat = nullptr;
	FilesPage *m_files = nullptr;
	ConsolePage *m_console = nullptr;
	GuestsPage *m_guests = nullptr;
	SessionHistory *m_history = nullptr;
	HistoryPage *m_historyPage = nullptr;
	QTimer m_saveTick;
	QTimer m_slowTick;
	WaterfallWidget *m_waterfall = nullptr;
	ConstellationWidget *m_constellation = nullptr;
	LevelMeter *m_vu = nullptr;
	LevelMeter *m_sn = nullptr;
	StatusLamps *m_lamps = nullptr;
	QPlainTextEdit *m_log = nullptr;

	QLabel *m_conn = nullptr;
	QLabel *m_frame = nullptr;
	QLabel *m_owner = nullptr;

	bool m_geometrySet = false;
	QString m_remoteCall;   /**< From APP_EV_STATE; see spinesource.h. */
};

#endif /* ARDOP_UI_STATIONWINDOW_H_ */
