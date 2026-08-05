#include "spinesource.h"

/**
 * @file spinesource.cpp
 * @brief Draining the embedded modem's queues (see spinesource.h).
 */

SpineSource::SpineSource(ModemThread *modem, QObject *parent)
	: PanelSource(parent), m_modem(modem)
{
	connect(&m_timer, &QTimer::timeout, this, &SpineSource::pump);
}

void SpineSource::start(int hz)
{
	if (hz < 1)
		hz = 1;
	m_timer.start(1000 / hz);
}

void SpineSource::stop()
{
	m_timer.stop();
}

void SpineSource::pump()
{
	if (!m_modem || !m_modem->spine())
		return;

	drainEvents();
	drainDisplay();

	/* The panel's "connected" indicator means something different for an
	 * embedded modem than for a remote one: there is no socket to lose, so
	 * what it reports is whether a device is open. */
	const app_dev_state st = m_modem->deviceState();
	const bool up = st == APP_DEV_RUNNING;
	if (up != m_wasRunning) {
		m_wasRunning = up;
		app_device_status ds {};
		m_modem->deviceStatus(&ds);
		emit connectionChanged(
			up, up ? QString("audio %1 Hz, %2x decimation")
					 .arg(ds.device_rate)
					 .arg(ds.ratio)
			       : QString::fromUtf8(ds.detail[0] ? ds.detail
								: "no audio device"));
	}

	emit pumped();
}

void SpineSource::drainDisplay()
{
	/*
	 * Everything queued, every tick. Coalesce by kind as app_display_pop
	 * documents: the last constellation, level and status win, but every
	 * spectrum row is a scan line and dropping one tears the waterfall.
	 */
	bool haveConstellation = false, haveAudio = false, haveStatus = false;
	ConstellationFrame lastFrame;
	float lastRms = 0.0f, lastPeak = 0.0f;
	LinkStatus lastStatus;

	while (app_display_pop(m_modem->spine(), &m_decoded)) {
		const ardop_telemetry &r = m_decoded.rec;
		switch (r.kind) {
		case ARDOP_TLM_SPECTRUM: {
			SpectrumRow row;
			row.mag.resize(ARDOP_BUSY_MAG_BINS);
			for (int i = 0; i < ARDOP_BUSY_MAG_BINS; i++)
				row.mag[i] = r.mag ? r.mag[i] : 0.0f;
			emit spectrum(row);
			break;
		}
		case ARDOP_TLM_CONSTELLATION: {
			lastFrame = ConstellationFrame {};
			lastFrame.frameType = r.frame_type;
			lastFrame.modulation = r.modulation;
			lastFrame.magThreshold = r.mag_threshold;
			lastFrame.phaseMrad.resize(r.n_points);
			lastFrame.mag.resize(r.n_points);
			for (int i = 0; i < r.n_points; i++) {
				lastFrame.phaseMrad[i] = r.phase_mrad[i];
				lastFrame.mag[i] = r.point_mag[i];
			}
			haveConstellation = true;
			break;
		}
		case ARDOP_TLM_FRAME:
			/*
			 * Not coalesced, unlike status. A status update is a
			 * mirror and only the last one matters; a frame record
			 * is an event, and dropping one loses a frame from the
			 * history for good.
			 */
			emit frameObserved(r);
			break;
		case ARDOP_TLM_AUDIO:
			lastRms = r.rms;
			lastPeak = r.peak;
			haveAudio = true;
			break;
		case ARDOP_TLM_STATUS:
			lastStatus.state = r.state;
			lastStatus.mode = r.mode;
			lastStatus.busy = r.busy;
			lastStatus.ptt = r.ptt;
			lastStatus.sn = r.sn;
			lastStatus.quality = r.quality;
			lastStatus.bandwidth = r.bandwidth;
			lastStatus.bufferLen = r.buffer_len;
			haveStatus = true;
			break;
		}
	}

	if (haveConstellation)
		emit constellation(lastFrame);
	if (haveAudio)
		emit audioLevel(lastRms, lastPeak);
	if (haveStatus)
		emit status(lastStatus);
}

void SpineSource::drainEvents()
{
	/* Lossless, and nothing else empties it: drain to exhaustion. */
	while (app_events_pop(m_modem->spine(), &m_event)) {
		const QString text = QString::fromUtf8(m_event.text);
		switch (m_event.kind) {
		case APP_EV_HOST_MSG:
			emit hostMessage(text);
			break;
		case APP_EV_REPLY:
			emit reply(text);
			break;
		case APP_EV_FAULT:
			emit fault(m_event.code, text);
			break;
		case APP_EV_DEVICE:
			emit deviceEvent(m_event.code, text);
			break;
		case APP_EV_STATE:
			emit linkState(m_event.code,
				       QString::fromUtf8(m_event.text));
			break;
		case APP_EV_GUEST:
			emit guestEvent(m_event.code,
					QString::fromUtf8(m_event.text));
			break;
		case APP_EV_LEADER:
			emit leaderDetected(text);
			break;
		case APP_EV_OWNER:
			emit ownerChanged(m_event.flag, text);
			break;
		case APP_EV_RX_DATA:
			emit received(QByteArray(m_event.tag),
				      QByteArray(reinterpret_cast<const char *>(
							 m_event.data),
						 int(m_event.data_len)));
			break;
		}
	}
}
