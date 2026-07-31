#ifndef ARDOP_GUI_MAINWINDOW_H_
#define ARDOP_GUI_MAINWINDOW_H_

#include <QLabel>
#include <QMainWindow>

#include "constellationwidget.h"
#include "gaugewidget.h"
#include "statuslamps.h"
#include "telemetryclient.h"
#include "waterfallwidget.h"

/**
 * @brief The instrument panel.
 *
 * Layout follows VARA's, because it is the one HF operators already know:
 * instruments across the middle, waterfall along the bottom, a status line
 * underneath. The constellation sits top-right where VARA puts it.
 *
 * Read-only. Nothing here can command the modem -- the telemetry stream is
 * one-way by construction, so a display cannot key a transmitter.
 */
class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow(const QString &host, quint16 port, QWidget *parent = nullptr);

private slots:
	void onSpectrum(const SpectrumRow &row);
	void onConstellation(const ConstellationFrame &f);
	void onAudio(float rms, float peak);
	void onStatus(const LinkStatus &st);
	void onConnectionChanged(bool up, const QString &detail);

private:
	TelemetryClient m_client;

	WaterfallWidget *m_waterfall = nullptr;
	ConstellationWidget *m_constellation = nullptr;
	GaugeWidget *m_vu = nullptr;
	GaugeWidget *m_sn = nullptr;
	StatusLamps *m_lamps = nullptr;
	QLabel *m_conn = nullptr;
	QLabel *m_frame = nullptr;

	bool m_geometrySet = false;
};

#endif /* ARDOP_GUI_MAINWINDOW_H_ */
