#include "stationwindow.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

extern "C" {
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
	: QMainWindow(parent), m_modem(modem), m_source(modem)
{
	m_tabs = new QTabWidget(this);
	m_tabs->addTab(buildPanel(), tr("Panel"));

	m_devices = new DevicesPage(modem, this);
	m_tabs->addTab(m_devices, tr("Devices"));

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

	connect(&m_source, &SpineSource::spectrum,
		this, &StationWindow::onSpectrum);
	connect(&m_source, &SpineSource::constellation,
		this, &StationWindow::onConstellation);
	connect(&m_source, &SpineSource::audioLevel,
		this, &StationWindow::onAudio);
	connect(&m_source, &SpineSource::status,
		this, &StationWindow::onStatus);
	connect(&m_source, &SpineSource::connectionChanged,
		this, &StationWindow::onConnectionChanged);
	connect(&m_source, &SpineSource::hostMessage,
		this, &StationWindow::onHostMessage);
	connect(&m_source, &SpineSource::reply,
		this, &StationWindow::onReply);
	connect(&m_source, &SpineSource::fault,
		this, &StationWindow::onFault);
	connect(&m_source, &SpineSource::deviceEvent,
		this, &StationWindow::onDeviceEvent);
	connect(&m_source, &SpineSource::ownerChanged,
		this, &StationWindow::onOwnerChanged);

	m_source.start(30);
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
		m_waterfall->setSpectrumGeometry(m_source.bins(),
						 m_source.firstBin(),
						 m_source.binHz());
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
}

void StationWindow::onReply(const QString &text)
{
	log(text);
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
