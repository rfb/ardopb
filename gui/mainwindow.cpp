#include "mainwindow.h"

#include <QHBoxLayout>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

/**
 * @file mainwindow.cpp
 * @brief The instrument panel layout and wiring (see mainwindow.h).
 */

namespace {
/* S/N band edges, matching the convention VARA established and that operators
 * already read: green above -10 dB, yellow down to -15, red below. */
constexpr double kSnGreen = -10.0;
constexpr double kSnRed = -15.0;

/* VU band edges in dBFS. Red starts at -6 because ARDOP's demodulator wants
 * headroom; a clipping input destroys the constellation before it shows up as
 * a decode failure. */
constexpr double kVuWarn = -12.0;
constexpr double kVuBad = -6.0;

double toDbfs(float linear)
{
	if (linear <= 1e-6f)
		return -60.0;
	return 20.0 * std::log10(double(linear));
}
}   // namespace

MainWindow::MainWindow(const QString &host, quint16 port, QWidget *parent)
	: QMainWindow(parent)
{
	auto *central = new QWidget(this);
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(6, 6, 6, 4);
	root->setSpacing(6);

	/* --- instrument row ---------------------------------------------- */
	auto *row = new QHBoxLayout;
	row->setSpacing(6);

	m_vu = new GaugeWidget(tr("VU"), central);
	m_vu->setScale(-60.0, 0.0, kVuWarn, kVuBad);
	m_vu->setUnknown(tr("Audio In: --"));

	m_lamps = new StatusLamps(central);
	m_lamps->setOffline();

	m_sn = new GaugeWidget(tr("S/N"), central);
	/* Reversed scale: high is good, so warn > bad. */
	m_sn->setScale(-25.0, 30.0, kSnGreen, kSnRed);
	m_sn->setUnknown(tr("S/N: --"));

	m_constellation = new ConstellationWidget(central);

	row->addWidget(m_vu, 2);
	row->addWidget(m_lamps, 3);
	row->addWidget(m_sn, 2);
	row->addWidget(m_constellation, 4);
	root->addLayout(row, 0);

	/* --- waterfall ---------------------------------------------------- */
	m_waterfall = new WaterfallWidget(central);
	root->addWidget(m_waterfall, 1);

	setCentralWidget(central);

	m_conn = new QLabel(tr("connecting…"), this);
	statusBar()->addWidget(m_conn);

	/* The mode of the last decoded frame, kept where it stays readable --
	 * the constellation's own label is small and low-contrast, and the mode
	 * is the thing you check when deciding whether the path can carry
	 * something faster. */
	m_frame = new QLabel(tr("no frames yet"), this);
	statusBar()->addPermanentWidget(m_frame);

	setWindowTitle(tr("ardop-gui — %1:%2").arg(host).arg(port));
	resize(900, 560);

	/* --- wiring -------------------------------------------------------- */
	connect(&m_client, &TelemetryClient::spectrum,
		this, &MainWindow::onSpectrum);
	connect(&m_client, &TelemetryClient::constellation,
		this, &MainWindow::onConstellation);
	connect(&m_client, &TelemetryClient::audioLevel,
		this, &MainWindow::onAudio);
	connect(&m_client, &TelemetryClient::status,
		this, &MainWindow::onStatus);
	connect(&m_client, &TelemetryClient::connectionChanged,
		this, &MainWindow::onConnectionChanged);

	m_client.start(host, port);
}

void MainWindow::onSpectrum(const SpectrumRow &row)
{
	if (!m_geometrySet) {
		m_waterfall->setSpectrumGeometry(m_client.bins(),
						 m_client.firstBin(),
						 m_client.binHz());
		m_geometrySet = true;
	}
	m_waterfall->addRow(row);
}

void MainWindow::onConstellation(const ConstellationFrame &f)
{
	m_constellation->setFrame(f);

	const QString mode = m_constellation->modeName();
	m_frame->setText(mode.isEmpty()
				 ? tr("frame 0x%1").arg(f.frameType, 2, 16,
						       QLatin1Char('0'))
				 : tr("mode: %1").arg(mode));
}

void MainWindow::onAudio(float rms, float peak)
{
	/* The needle follows RMS (what the ear and the demodulator care about)
	 * while the caption also reports peak, which is what clips. */
	const double rmsDb = toDbfs(rms);
	m_vu->setValue(rmsDb, tr("%1 dB  pk %2")
				      .arg(rmsDb, 0, 'f', 1)
				      .arg(toDbfs(peak), 0, 'f', 1));
}

void MainWindow::onStatus(const LinkStatus &st)
{
	m_lamps->setStatus(st);

	/* S/N is measured on a frame's leader, so it is a per-frame figure and
	 * stays put between frames rather than decaying to nothing. */
	if (st.sn != 0 || st.quality != 0)
		m_sn->setValue(st.sn, tr("%1 dB   Q %2")
					      .arg(st.sn).arg(st.quality));
}

void MainWindow::onConnectionChanged(bool up, const QString &detail)
{
	m_conn->setText(detail);
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
