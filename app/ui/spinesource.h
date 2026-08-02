#ifndef ARDOP_UI_SPINESOURCE_H_
#define ARDOP_UI_SPINESOURCE_H_

#include <QObject>
#include <QString>
#include <QTimer>

#include "modemthread.h"
#include "telemetryclient.h"

extern "C" {
#include "shell/telemetry.h"
}

/**
 * @file spinesource.h
 * @brief The embedded modem, presented the way the remote one already is.
 *
 * `TelemetryClient` reads a modem's telemetry socket and re-emits it as Qt
 * signals. The instrument widgets were written against those signals, and they
 * work -- so the cheapest way to put them in front of an *embedded* modem is to
 * emit the same five signals from the in-process queue instead of from a socket.
 *
 * That is possible because the spine's display queue carries the **wire format**
 * rather than structs: `app_display_pop` hands back an `ardop_tlm_decoded`,
 * which is exactly what `TelemetryClient` produces from the stream. The two
 * sources are interchangeable, and the panel does not know which it has --
 * which is also what makes `--remote` a mode of this binary rather than a
 * second program.
 *
 * ## Draining is the interface thread's job
 *
 * `spine.h` divides its API by thread: the modem thread produces, and exactly
 * one other thread consumes. This is that other thread. The pump runs on a Qt
 * timer at roughly frame rate and drains **everything** each tick, because both
 * upward queues are bounded and the event one is lossless -- nothing else empties
 * them.
 *
 * Coalescing follows `app_display_pop`'s contract: constellation, audio and
 * status are whole-value replacements so only the last matters, but every
 * spectrum record is one scan line of the waterfall and skipping them tears the
 * picture.
 */
class SpineSource : public QObject {
	Q_OBJECT

public:
	explicit SpineSource(ModemThread *modem, QObject *parent = nullptr);

	/** @brief Start pumping at @p hz. */
	void start(int hz = 30);
	void stop();

	/* Geometry, for the waterfall's frequency axis. Fixed for an embedded
	 * modem: it is the same build, so the constants are ours. */
	int bins() const { return ARDOP_BUSY_MAG_BINS; }
	int firstBin() const { return ARDOP_BUSY_FIRST_BIN; }
	float binHz() const { return ARDOP_BUSY_BIN_HZ; }

signals:
	/* Deliberately identical to TelemetryClient's, so the widgets bind to
	 * either without knowing which. */
	void spectrum(const SpectrumRow &row);
	void constellation(const ConstellationFrame &frame);
	void audioLevel(float rms, float peak);
	void status(const LinkStatus &st);
	void connectionChanged(bool up, const QString &detail);

	/* The half a socket cannot carry: an embedded modem talks back. */
	void hostMessage(const QString &text);
	void reply(const QString &text);
	void fault(int code, const QString &text);
	void deviceEvent(int code, const QString &text);
	void ownerChanged(bool tncAttached, const QString &text);
	void received(const QByteArray &tag, const QByteArray &data);

	/**
	 * @brief The link state changed, and who it changed with.
	 *
	 * analysis/16 §7: the remote callsign is the first thing an operator
	 * looks for and no telemetry record carries it. Embedded, the event bus
	 * has it. **TelemetryClient has no equivalent**, which is why this signal
	 * sits below the shared five rather than among them -- a remote panel
	 * shows the state without the callsign until §7's second half is built.
	 */
	void linkState(int state, const QString &remote);

private slots:
	void pump();

private:
	void drainDisplay();
	void drainEvents();

	ModemThread *m_modem = nullptr;
	QTimer m_timer;
	bool m_wasRunning = false;

	/* Decoded into members rather than locals: between them these are around
	 * ten kilobytes of arrays, and the pump runs thirty times a second. */
	ardop_tlm_decoded m_decoded {};
	app_event m_event {};
};

#endif /* ARDOP_UI_SPINESOURCE_H_ */
