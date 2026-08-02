#include "telemetryclient.h"

/**
 * @file telemetryclient.cpp
 * @brief The telemetry stream reader (see telemetryclient.h).
 *
 * Framing is delegated to the modem's own ardop_tlm_parse(), so there is no
 * second implementation of the format to drift.
 */

namespace {
/* Long enough not to hammer a daemon that is not running, short enough that
 * starting one feels immediate. */
constexpr int kRetryMs = 1000;

/* If the buffer ever grows past this without yielding a record the stream is
 * not what we think it is; drop it and resynchronise by reconnecting. */
constexpr int kMaxBuffer = 4 * ARDOP_TLM_MAX_RECORD;
}   // namespace

TelemetryClient::TelemetryClient(QObject *parent)
	: PanelSource(parent)
{
	connect(&m_sock, &QTcpSocket::readyRead,
		this, &TelemetryClient::onReadyRead);
	connect(&m_sock, &QTcpSocket::connected,
		this, &TelemetryClient::onConnected);
	connect(&m_sock, &QTcpSocket::disconnected,
		this, &TelemetryClient::onDisconnected);
	connect(&m_sock, &QAbstractSocket::errorOccurred,
		this, [this](QAbstractSocket::SocketError) {
			onDisconnected();
		});

	m_retry.setInterval(kRetryMs);
	connect(&m_retry, &QTimer::timeout, this, &TelemetryClient::retry);
}

void TelemetryClient::start(const QString &host, quint16 port)
{
	m_host = host;
	m_port = port;
	retry();
}

void TelemetryClient::retry()
{
	if (m_sock.state() != QAbstractSocket::UnconnectedState)
		return;
	m_sock.connectToHost(m_host, m_port);
}

void TelemetryClient::onConnected()
{
	m_retry.stop();
	m_buf.clear();
	m_greeted = false;
	emit connectionChanged(true, tr("connected to %1:%2")
					     .arg(m_host).arg(m_port));
}

void TelemetryClient::onDisconnected()
{
	bool was = m_greeted;
	m_greeted = false;
	m_buf.clear();
	if (was)
		emit connectionChanged(false, tr("disconnected"));
	if (!m_retry.isActive())
		m_retry.start();
}

void TelemetryClient::onReadyRead()
{
	m_buf.append(m_sock.readAll());
	consume();

	if (m_buf.size() > kMaxBuffer) {
		/* Unparseable: better to reconnect than to keep growing. */
		emit connectionChanged(false, tr("stream out of sync"));
		m_sock.abort();
		onDisconnected();
	}
}

void TelemetryClient::consume()
{
	const auto *raw = reinterpret_cast<const uint8_t *>(m_buf.constData());
	size_t avail = static_cast<size_t>(m_buf.size());
	size_t pos = 0;

	if (!m_greeted) {
		uint16_t bins = 0, first = 0;
		float hz = 0.0f;
		if (avail < ARDOP_TLM_HELLO_LEN)
			return;
		if (!ardop_tlm_parse_hello(raw, avail, &bins, &first, &hz)) {
			emit connectionChanged(false,
					       tr("not an ardopb telemetry "
						  "stream (bad hello)"));
			m_sock.abort();
			return;
		}
		m_bins = bins;
		m_firstBin = first;
		m_binHz = hz;
		m_greeted = true;
		pos = ARDOP_TLM_HELLO_LEN;
		emit connectionChanged(true, tr("streaming: %1 bins from %2 Hz")
						     .arg(bins)
						     .arg(first * hz, 0, 'f', 0));
	}

	for (;;) {
		size_t consumed = 0;
		bool ok = ardop_tlm_parse(raw + pos, avail - pos, &m_decoded,
					  &consumed);
		if (consumed == 0)
			break;   /* incomplete: wait for more bytes. */
		pos += consumed;

		if (!ok)
			continue;   /* unknown kind from a newer daemon. */

		const ardop_telemetry &r = m_decoded.rec;
		switch (r.kind) {
		case ARDOP_TLM_SPECTRUM: {
			SpectrumRow row;
			row.mag.resize(m_bins);
			for (int i = 0; i < m_bins; i++)
				row.mag[i] = r.mag[i];
			emit spectrum(row);
			break;
		}
		case ARDOP_TLM_CONSTELLATION: {
			ConstellationFrame f;
			f.frameType = r.frame_type;
			f.modulation = r.modulation;
			f.magThreshold = r.mag_threshold;
			f.phaseMrad.resize(r.n_points);
			f.mag.resize(r.n_points);
			for (int i = 0; i < r.n_points; i++) {
				f.phaseMrad[i] = r.phase_mrad[i];
				f.mag[i] = r.point_mag[i];
			}
			emit constellation(f);
			break;
		}
		case ARDOP_TLM_AUDIO:
			emit audioLevel(r.rms, r.peak);
			break;
		case ARDOP_TLM_STATUS: {
			LinkStatus st;
			st.state = r.state;
			st.mode = r.mode;
			st.busy = r.busy;
			st.ptt = r.ptt;
			st.sn = r.sn;
			st.quality = r.quality;
			st.bandwidth = r.bandwidth;
			st.bufferLen = r.buffer_len;
			emit status(st);
			break;
		}
		}
	}

	if (pos > 0)
		m_buf.remove(0, static_cast<int>(pos));
}
