#include "stationwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QPushButton>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

extern "C" {
#include "shell/build.h"
#include "shell/host.h"
#include "shell/settings.h"
}

/**
 * @file stationwindow.cpp
 * @brief The station window (see stationwindow.h).
 */

namespace {
/* The same band edges the standalone panel uses, and for the same reasons --
 * see gui/mainwindow.cpp. Duplicated rather than shared because they are
 * presentation constants of one layout, and the layouts will diverge. */
/*
 * Where a reading is supposed to sit, rather than where it becomes a warning.
 *
 * The dials took a warn/bad pair and were green everywhere below the warning --
 * which told an operator with 40 dB too little audio that everything was fine.
 * §9 replaced that with a band: below kVuBandLo is as wrong as above kVuBandHi,
 * and looks it.
 *
 * S/N has a low edge and no high one, because more is simply better. Its band
 * top is the top of the scale.
 */
constexpr double kVuBandLo = -24.0;
constexpr double kVuBandHi = -12.0;
constexpr double kSnBandLo = 5.0;

double toDbfs(float linear)
{
	if (linear <= 1e-6f)
		return -60.0;
	return 20.0 * std::log10(double(linear));
}
}   // namespace

StationWindow::StationWindow(ModemThread *modem, QWidget *parent)
	: QMainWindow(parent), m_modem(modem)
{
	m_spineSource = new SpineSource(modem, this);
	m_source = m_spineSource;

	m_tabs = new QTabWidget(this);
	m_tabs->addTab(buildPanel(), tr("Panel"));

	m_devices = new DevicesPage(modem, this);
	m_tabs->addTab(m_devices, tr("Devices"));

	/* Station before Console: the order is the order an operator needs them
	 * in, which is also the order of how often they are opened. */
	m_station = new StationPage(modem, this);
	m_tabs->addTab(m_station, tr("Station"));

	/* Chat and Files sit next to Station because they are what a session is
	 * *for*: the Station screen starts a connection, and these two are the
	 * only screens that put anything through it. */
	m_asp = new AspSession(modem, this);
	m_chat = new ChatPage(m_asp, this);
	m_tabs->addTab(m_chat, tr("Chat"));
	m_files = new FilesPage(m_asp, this);
	m_tabs->addTab(m_files, tr("Files"));

	m_console = new ConsolePage(modem, this);
	m_tabs->addTab(m_console, tr("Console"));

	/* The history is fed from the display queue, so it exists wherever the
	 * panel does -- but only the embedded build has a tab for it today; see
	 * the remote constructor. */
	m_history = new SessionHistory(this);
	m_historyPage = new HistoryPage(m_history, this);
	m_tabs->addTab(m_historyPage, tr("History"));

	m_guests = new GuestsPage(modem, this);
	m_tabs->addTab(m_guests, tr("Guests"));

	setCentralWidget(m_tabs);

	/* One menu, one item. A window with no menu bar has nowhere to put the
	 * build identifier, and a tester needs it in the first line of every
	 * report. */
	menuBar()->addMenu(tr("&Help"))
		->addAction(tr("&About"), this, &StationWindow::showAbout);

	/* The device state changes on the modem thread and has no event of its
	 * own for every transition, so the screen re-reads it a few times a
	 * second. Slower than the panel: this is a status line, not an
	 * instrument. */
	connect(&m_slowTick, &QTimer::timeout, m_devices,
		&DevicesPage::updateStatus);
	m_slowTick.start(250);

	/* A selection the operator applied is one they will want next time.
	 * Unknown keys in the file are preserved, so writing ours back cannot
	 * disturb anybody else's. */
	connect(m_devices, &DevicesPage::selectionApplied, this,
		[this](const app_device_selection &sel) {
			char path[512];
			if (!ardop_settings_path(path, sizeof path))
				return;
			ardop_settings s {};
			ardop_settings_load(&s, path);
			if (app_devices_selection_store(&sel, &s) &&
			    ardop_settings_save(&s, path))
				log(tr("device selection saved"));
		});

	m_conn = new QLabel(tr("no audio device"), this);
	statusBar()->addWidget(m_conn);

	m_owner = new QLabel(QString(), this);
	statusBar()->addPermanentWidget(m_owner);

	m_frame = new QLabel(tr("no frames yet"), this);
	statusBar()->addPermanentWidget(m_frame);

	setWindowTitle(tr("ardop station"));
	resize(980, 640);

	/*
	 * The build identifier, in the log, at start-up.
	 *
	 * The Help menu shows it too, and a person can copy it from there. This
	 * line exists because a tester is asked to attach the panel log, and a
	 * log that carries its own build identifier answers the first question
	 * of every fault report without anybody having to remember to ask it.
	 */
	{
		char line[160];
		log(QString::fromUtf8(
			ardop_build_line("ardop-station", line, sizeof line)));
	}

	connectPanel();

	/* The commanding half. These have no counterpart on a telemetry stream,
	 * which is why they are connected here and not in connectPanel. */
	connect(m_spineSource, &SpineSource::hostMessage,
		this, &StationWindow::onHostMessage);
	connect(m_spineSource, &SpineSource::reply,
		this, &StationWindow::onReply);
	connect(m_spineSource, &SpineSource::fault,
		this, &StationWindow::onFault);
	connect(m_spineSource, &SpineSource::deviceEvent,
		this, &StationWindow::onDeviceEvent);
	connect(m_spineSource, &SpineSource::ownerChanged,
		this, &StationWindow::onOwnerChanged);
	connect(m_spineSource, &SpineSource::linkState,
		this, &StationWindow::onLinkState);
	connect(m_spineSource, &SpineSource::frameObserved, m_history,
		&SessionHistory::append);
	connect(m_spineSource, &SpineSource::guestEvent,
		this, &StationWindow::onGuestEvent);
	connect(m_guests, &GuestsPage::message, this, &StationWindow::log);
	connect(m_guests, &GuestsPage::settingsChanged, this,
		[this] { m_saveTick.start(); });

	connect(m_station, &StationPage::message, this, &StationWindow::log);

	/*
	 * The application protocol, driven entirely from signals this window was
	 * already carrying.
	 *
	 * The order inside the pump is what makes one tick enough: `received`
	 * fires while the event queue is being drained, and `pumped` fires after
	 * both queues are empty -- so a message that arrives is parsed, and the
	 * answer it provokes is offered to the transmit queue, in the same
	 * thirtieth of a second.
	 */
	connect(m_spineSource, &SpineSource::received, m_asp,
		&AspSession::onReceived);
	connect(m_spineSource, &SpineSource::pumped, m_asp, &AspSession::service);
	connect(m_spineSource, &SpineSource::linkState, m_asp,
		&AspSession::onLinkState);
	connect(m_spineSource, &SpineSource::ownerChanged, m_asp,
		&AspSession::onOwnerChanged);

	connect(m_asp, &AspSession::stateChanged, m_chat,
		&ChatPage::onStateChanged);
	connect(m_asp, &AspSession::stateChanged, m_files,
		&FilesPage::onStateChanged);
	connect(m_asp, &AspSession::textArrived, m_chat, &ChatPage::onTextArrived);
	connect(m_asp, &AspSession::binaryArrived, m_chat,
		&ChatPage::onBinaryArrived);
	connect(m_asp, &AspSession::offerArrived, m_files,
		&FilesPage::onOfferArrived);
	connect(m_asp, &AspSession::progress, m_files, &FilesPage::onProgress);
	connect(m_asp, &AspSession::transferDone, m_files,
		&FilesPage::onTransferDone);

	/*
	 * A note goes to all three: the transfer log because that is where it
	 * belongs, the chat because a session-level problem is what explains a
	 * message that never got an answer, and the panel log because an
	 * operator with neither tab open should still see it.
	 */
	connect(m_asp, &AspSession::note, m_files, &FilesPage::onNote);
	connect(m_asp, &AspSession::note, m_chat, &ChatPage::onNote);
	connect(m_asp, &AspSession::note, this, &StationWindow::log);

	connect(m_files, &FilesPage::settingsChanged, this,
		[this] { m_saveTick.start(); });

	/* HELLO carries the callsign, so the protocol follows the *station's*
	 * callsign rather than the saved one -- which is why this is its own
	 * signal and not settingsChanged. A guest that sets MYCALL changes what
	 * this station is on the air, and the next session introduces itself
	 * that way; the settings file still keeps the operator's. */
	connect(m_station, &StationPage::callsignChanged, m_asp,
		&AspSession::setCallsign);

	/*
	 * Settings are written on a timer rather than on every keystroke.
	 *
	 * Every field on the Station screen applies to the modem the moment it
	 * changes -- that part must not be delayed. Writing the file is a
	 * different question: a rewrite per character would put an atomic
	 * replace of the config file in the middle of an operator typing their
	 * callsign. One second after the last change is soon enough, and the
	 * window also saves on close (see closeEvent) so nothing is lost by
	 * waiting.
	 */
	m_saveTick.setSingleShot(true);
	m_saveTick.setInterval(1000);
	connect(&m_saveTick, &QTimer::timeout, this, &StationWindow::saveStation);
	connect(m_station, &StationPage::settingsChanged, this,
		[this] { m_saveTick.start(); });

	m_spineSource->start(30);
}

StationWindow::StationWindow(const QString &host, quint16 port, QWidget *parent)
	: QMainWindow(parent)
{
	m_client = new TelemetryClient(this);
	m_source = m_client;

	/*
	 * One screen, and no tab bar around it.
	 *
	 * A single-tab QTabWidget is a frame with a decoration that promises
	 * more. What this window can do remotely is show the panel, so it shows
	 * the panel -- and the title carries where it is pointed, because a
	 * display watching somebody else's station should never leave that in
	 * doubt.
	 */
	setCentralWidget(buildPanel());

	menuBar()->addMenu(tr("&Help"))
		->addAction(tr("&About"), this, &StationWindow::showAbout);

	m_conn = new QLabel(tr("connecting to %1:%2").arg(host).arg(port), this);
	statusBar()->addWidget(m_conn);
	m_owner = new QLabel(QString(), this);
	statusBar()->addPermanentWidget(m_owner);
	m_frame = new QLabel(tr("no frames yet"), this);
	statusBar()->addPermanentWidget(m_frame);

	setWindowTitle(tr("ardop panel - %1:%2").arg(host).arg(port));
	resize(980, 640);

	connectPanel();
	m_client->start(host, port);
}

/* The five a display needs, from whichever source this window was built with. */
void StationWindow::connectPanel()
{
	connect(m_source, &PanelSource::spectrum,
		this, &StationWindow::onSpectrum);
	connect(m_source, &PanelSource::constellation,
		this, &StationWindow::onConstellation);
	connect(m_source, &PanelSource::audioLevel,
		this, &StationWindow::onAudio);
	connect(m_source, &PanelSource::status,
		this, &StationWindow::onStatus);
	connect(m_source, &PanelSource::connectionChanged,
		this, &StationWindow::onConnectionChanged);
}

QWidget *StationWindow::buildPanel()
{
	auto *page = new QWidget(this);
	auto *root = new QVBoxLayout(page);
	root->setContentsMargins(6, 6, 6, 4);
	root->setSpacing(6);

	auto *row = new QHBoxLayout;
	row->setSpacing(6);

	/*
	 * The two meters stack vertically in the space one dial used to take.
	 *
	 * §9's band edges, and the comment is the point: the dials were green
	 * everywhere below -12 dB, so a signal 40 dB too quiet looked correct.
	 * Audio level is two-sided -- too quiet and too loud are both faults.
	 * S/N is one-sided, which is said by putting the band's top edge at the
	 * top of the scale rather than by having a second kind of meter.
	 */
	auto *meters = new QVBoxLayout;
	meters->setSpacing(4);

	m_vu = new LevelMeter(tr("RX"), page);
	m_vu->setScale(-60.0, 0.0);
	m_vu->setBand(kVuBandLo, kVuBandHi);
	m_vu->setUnknown(tr("no audio"));

	m_sn = new LevelMeter(tr("S/N"), page);
	m_sn->setScale(-25.0, 30.0);
	m_sn->setBand(kSnBandLo, 30.0);
	m_sn->setUnknown(tr("--"));

	meters->addWidget(m_vu);
	meters->addWidget(m_sn);
	meters->addStretch(1);

	m_lamps = new StatusLamps(page);
	m_lamps->setOffline();

	m_constellation = new ConstellationWidget(page);

	row->addLayout(meters, 5);
	row->addWidget(m_lamps, 3);
	row->addWidget(m_constellation, 4);
	root->addLayout(row, 0);

	m_waterfall = new WaterfallWidget(page);
	root->addWidget(m_waterfall, 1);

	/* The modem's own running commentary. An embedded modem can talk back --
	 * the remote panel has no channel for this at all -- and the first thing
	 * an operator setting a station up needs is to see what it said. */
	m_log = new QPlainTextEdit(page);
	m_log->setReadOnly(true);
	m_log->setMaximumBlockCount(500);
	m_log->setMaximumHeight(120);
	root->addWidget(m_log, 0);

	return page;
}

void StationWindow::applySavedSelection(const app_device_selection &sel)
{
	m_devices->setSelection(sel);
	if (m_modem->requestDevices(sel))
		log(tr("opening the saved devices…"));
}

void StationWindow::log(const QString &line)
{
	m_log->appendPlainText(
		QDateTime::currentDateTime().toString("HH:mm:ss ") + line);
}

void StationWindow::onSpectrum(const SpectrumRow &row)
{
	if (!m_geometrySet) {
		m_waterfall->setSpectrumGeometry(m_source->bins(),
						 m_source->firstBin(),
						 m_source->binHz());
		m_geometrySet = true;
	}
	m_waterfall->addRow(row);
}

void StationWindow::onConstellation(const ConstellationFrame &f)
{
	m_constellation->setFrame(f);
	const QString mode = m_constellation->modeName();
	m_frame->setText(mode.isEmpty()
				 ? tr("frame 0x%1").arg(f.frameType, 2, 16,
						       QLatin1Char('0'))
				 : tr("mode: %1").arg(mode));
}

void StationWindow::onAudio(float rms, float peak)
{
	const double rmsDb = toDbfs(rms);
	const double peakDb = toDbfs(peak);
	m_vu->setValue(rmsDb, peakDb);
	m_vu->setCaption(tr("%1 dB  pk %2")
				 .arg(rmsDb, 0, 'f', 1)
				 .arg(peakDb, 0, 'f', 1));
}

void StationWindow::onStatus(const LinkStatus &st)
{
	m_lamps->setStatus(st);
	if (m_station)
		m_station->setLinkState(
			QString::fromUtf8(
				ardop_host_state_name((ardop_link_state)st.state)),
			m_remoteCall);
	if (st.sn != 0 || st.quality != 0) {
		/* One-sided: the same number twice, so nothing draws a peak
		 * marker for a quantity that has no transient meaning. */
		m_sn->setValue(st.sn, st.sn);
		m_sn->setCaption(tr("%1 dB   Q %2").arg(st.sn).arg(st.quality));
	}
}

void StationWindow::onConnectionChanged(bool up, const QString &detail)
{
	m_conn->setText(detail);
	log(up ? tr("audio: %1").arg(detail) : tr("audio stopped: %1").arg(detail));
	if (!up) {
		m_frame->setText(tr("no frames yet"));
		m_lamps->setOffline();
		m_vu->setUnknown(tr("no audio"));
		m_sn->setUnknown(tr("--"));
		m_constellation->clear();
		m_waterfall->clear();
		m_geometrySet = false;

		/* The frame timestamps are elapsed samples on a clock that
		 * belongs to the device. A new device restarts it, so keeping
		 * the old records would put the session in the wrong order --
		 * and silently, which is the worst way for a history to be
		 * wrong. */
		if (m_history)
			m_history->clear();
	}
}

void StationWindow::onHostMessage(const QString &text)
{
	log(text);
	m_console->appendMessage(text);
}

void StationWindow::onReply(const QString &text)
{
	/*
	 * Replies go to the Console in full and to the panel log as well.
	 *
	 * They are the modem answering a question, and the Console is where the
	 * question is visible -- but an operator who never opens the Console
	 * should still see that something replied, because the commonest reply
	 * worth noticing is a FAULT.
	 */
	log(text);
	m_console->appendReply(text);
}

void StationWindow::onFault(int code, const QString &text)
{
	(void)code;
	log(tr("FAULT: %1").arg(text));
}

void StationWindow::onDeviceEvent(int code, const QString &text)
{
	(void)code;
	log(text);
}

void StationWindow::onGuestEvent(int code, const QString &text)
{
	m_guests->onGuestEvent(code, text);

	/*
	 * A guest's setting change reaches the Station screen, so the two agree.
	 * The event text is "COMMAND -> REPLY"; the reply is the half that says
	 * what the modem actually did.
	 */
	if (code == APP_GUEST_COMMAND && m_station) {
		const int arrow = text.indexOf(QLatin1String(" -> "));
		if (arrow > 0)
			m_station->applyExternalChange(text.mid(arrow + 4));
	}

	/* The ones an operator needs even with the Guests tab closed: something
	 * attached, something was turned away, or the port would not bind. */
	if (code != APP_GUEST_COMMAND)
		log(text);
}

void StationWindow::onOwnerChanged(bool attached, const QString &text)
{
	/*
	 * A guest owning the link is not a footnote: while it holds, this
	 * program's own transmission is refused, and an operator wondering why
	 * the send button does nothing deserves to see why without reading a log.
	 */
	m_owner->setText(attached ? tr("TNC client owns the link") : QString());
	if (m_station)
		m_station->setGuestOwned(attached);
	if (m_guests)
		m_guests->setAttached(attached);
	log(text);
}

void StationWindow::onLinkState(int state, const QString &remote)
{
	m_remoteCall = remote;
	m_station->setLinkState(
		QString::fromUtf8(ardop_host_state_name((ardop_link_state)state)),
		remote);

	/* The status bar gains the one field the telemetry record does not
	 * carry, next to the state it belongs with. */
	m_conn->setText(remote.isEmpty()
				? m_conn->text()
				: tr("connected to %1").arg(remote));
}

void StationWindow::applySavedStation(const ardop_settings *s)
{
	m_station->applySaved(s);
	m_guests->applySaved(s);
	m_files->applySaved(s);

	/* After the station, so the callsign it just loaded is the one the
	 * protocol will introduce itself with. */
	m_asp->setCallsign(m_station->callsign());
}

/*
 * The debounce had no flush, and the comment above claimed it did.
 *
 * One second is a good interval for not rewriting the config file per keystroke
 * and a bad one for being the last second of the program's life: an operator who
 * picks a receive folder and closes the window loses it, and it is exactly the
 * kind of loss that gets blamed on the setting not being saved at all.
 */
void StationWindow::closeEvent(QCloseEvent *event)
{
	if (m_saveTick.isActive()) {
		m_saveTick.stop();
		saveStation();
	}
	QMainWindow::closeEvent(event);
}

void StationWindow::showAbout()
{
	char line[160];
	const QString build = QString::fromUtf8(
		ardop_build_line("ardop-station", line, sizeof line));

	QDialog dlg(this);
	dlg.setWindowTitle(tr("About ardop-station"));
	auto *box = new QVBoxLayout(&dlg);

	auto *title = new QLabel(tr("<b>ardop-station</b>"), &dlg);
	box->addWidget(title);

	auto *what = new QLabel(
		tr("An HF data station: the modem, the radio, the link, and "
		   "what you wanted to send."),
		&dlg);
	what->setWordWrap(true);
	box->addWidget(what);

	/* Selectable, because the next thing a person does with this text is
	 * paste it into a fault report. */
	auto *info = new QLabel(build, &dlg);
	info->setTextInteractionFlags(Qt::TextSelectableByMouse);
	info->setWordWrap(true);
	box->addWidget(info);

	auto *qt = new QLabel(tr("Qt %1").arg(QLatin1String(qVersion())), &dlg);
	qt->setTextInteractionFlags(Qt::TextSelectableByMouse);
	box->addWidget(qt);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	auto *copy = buttons->addButton(tr("Copy build details"),
					QDialogButtonBox::ActionRole);
	connect(copy, &QPushButton::clicked, this, [build] {
		QApplication::clipboard()->setText(build);
	});
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	box->addWidget(buttons);

	dlg.exec();
}

void StationWindow::saveStation()
{
	char path[512];
	if (!ardop_settings_path(path, sizeof path))
		return;

	/* Load first: the store preserves keys it does not know, so writing our
	 * section back cannot disturb the device selection or anybody else's
	 * future section. */
	ardop_settings s {};
	ardop_settings_load(&s, path);
	m_station->store(&s);
	m_guests->store(&s);
	m_files->store(&s);
	if (!ardop_settings_save(&s, path))
		log(tr("could not save settings to %1")
			    .arg(QString::fromUtf8(path)));
}
