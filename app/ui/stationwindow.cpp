#include "stationwindow.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

extern "C" {
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
constexpr double kSnGreen = -10.0;
constexpr double kSnRed = -15.0;
constexpr double kVuWarn = -12.0;
constexpr double kVuBad = -6.0;

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

	m_console = new ConsolePage(modem, this);
	m_tabs->addTab(m_console, tr("Console"));

	setCentralWidget(m_tabs);

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

	connect(m_station, &StationPage::message, this, &StationWindow::log);

	/*
	 * Settings are written on a timer rather than on every keystroke.
	 *
	 * Every field on the Station screen applies to the modem the moment it
	 * changes -- that part must not be delayed. Writing the file is a
	 * different question: a rewrite per character would put an atomic
	 * replace of the config file in the middle of an operator typing their
	 * callsign. One second after the last change is soon enough, and the
	 * window also saves on close so nothing is lost by waiting.
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

	m_vu = new GaugeWidget(tr("VU"), page);
	m_vu->setScale(-60.0, 0.0, kVuWarn, kVuBad);
	m_vu->setUnknown(tr("Audio In: --"));

	m_lamps = new StatusLamps(page);
	m_lamps->setOffline();

	m_sn = new GaugeWidget(tr("S/N"), page);
	m_sn->setScale(-25.0, 30.0, kSnGreen, kSnRed);
	m_sn->setUnknown(tr("S/N: --"));

	m_constellation = new ConstellationWidget(page);

	row->addWidget(m_vu, 2);
	row->addWidget(m_lamps, 3);
	row->addWidget(m_sn, 2);
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
	m_vu->setValue(rmsDb, tr("%1 dB  pk %2")
				      .arg(rmsDb, 0, 'f', 1)
				      .arg(toDbfs(peak), 0, 'f', 1));
}

void StationWindow::onStatus(const LinkStatus &st)
{
	m_lamps->setStatus(st);
	if (m_station)
		m_station->setLinkState(
			QString::fromUtf8(
				ardop_host_state_name((ardop_link_state)st.state)),
			m_remoteCall);
	if (st.sn != 0 || st.quality != 0)
		m_sn->setValue(st.sn, tr("%1 dB   Q %2")
					      .arg(st.sn).arg(st.quality));
}

void StationWindow::onConnectionChanged(bool up, const QString &detail)
{
	m_conn->setText(detail);
	log(up ? tr("audio: %1").arg(detail) : tr("audio stopped: %1").arg(detail));
	if (!up) {
		m_frame->setText(tr("no frames yet"));
		m_lamps->setOffline();
		m_vu->setUnknown(tr("Audio In: --"));
		m_sn->setUnknown(tr("S/N: --"));
		m_constellation->clear();
		m_waterfall->clear();
		m_geometrySet = false;
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

void StationWindow::onOwnerChanged(bool attached, const QString &text)
{
	/*
	 * A guest owning the link is not a footnote: while it holds, this
	 * program's own transmission is refused, and an operator wondering why
	 * the send button does nothing deserves to see why without reading a log.
	 */
	m_owner->setText(attached ? tr("TNC client owns the link") : QString());
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
	if (!ardop_settings_save(&s, path))
		log(tr("could not save settings to %1")
			    .arg(QString::fromUtf8(path)));
}
