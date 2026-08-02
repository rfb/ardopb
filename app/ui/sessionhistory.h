#ifndef ARDOP_UI_SESSIONHISTORY_H_
#define ARDOP_UI_SESSIONHISTORY_H_

#include <QObject>
#include <QString>
#include <QVector>

extern "C" {
#include "shell/telemetry.h"
}

/**
 * @file sessionhistory.h
 * @brief Every frame this station has sent or heard, in order.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §10. The panel shows what
 * the modem is doing *now*; this is what it has been doing, which is the
 * question an operator actually has when something went wrong five minutes ago.
 *
 * ## Turn time is computed here, not in a view
 *
 * It needs the *previous* record, and a view should not have to look backwards
 * to draw a row. So it is computed once on insert and stored.
 *
 * What it measures is the gap between a frame being decoded and the next
 * transmission starting -- which is the quantity
 * [15](../../analysis/15-platform-audio-and-ptt.md) §8's 250 ms budget is about.
 * Kept in samples; divided by the sample rate only for display.
 *
 * ## Bounded, like everything else here
 *
 * A fixed ring that never reallocates. When it wraps, the oldest record is
 * dropped and ::wrapped is emitted so a table can rebase its row indices --
 * which is why indices here are always "0 is the oldest still held" and never
 * an absolute frame number.
 */
class SessionHistory : public QObject {
	Q_OBJECT

public:
	/** @brief One frame, as the history keeps it. */
	struct Record {
		quint64 at = 0;        /**< Elapsed samples when observed. */
		quint8 frameType = 0;
		quint8 dir = 0;        /**< ::ardop_tlm_dir. */
		qint16 quality = -1;   /**< RX only; -1 when not applicable. */
		qint16 sn = -1;
		quint32 turnMs = 0;    /**< 0 when not a turnaround. */
	};

	/**
	 * @brief Long enough for a full Winlink session.
	 *
	 * At 24 bytes a record this is around a quarter of a megabyte, which is
	 * a rounding error beside the 300 kB runtime and buys a history nobody
	 * runs off the end of in an afternoon.
	 */
	static constexpr int kCapacity = 10000;

	explicit SessionHistory(QObject *parent = nullptr);

	/** @brief Add a frame. Computes ::Record::turnMs, then stores. */
	void append(const ardop_telemetry &rec);

	int count() const { return m_count; }
	const Record &at(int i) const;

	/** @brief Forget everything. For a new session, or a device change. */
	void clear();

signals:
	/** @brief A record was added at @p index. */
	void appended(int index);

	/** @brief The oldest record was dropped; every index shifted down one. */
	void wrapped();

	/**
	 * @brief Everything was discarded.
	 *
	 * Separate from ::wrapped because a view must respond differently: a
	 * wrap shifts indices by one, a clear invalidates all of them. A model
	 * that treated this as a wrap would keep handing a view row numbers that
	 * no longer address anything.
	 */
	void cleared();

private:
	QVector<Record> m_ring;
	int m_head = 0;      /**< Where the next record goes. */
	int m_count = 0;

	/* When the last non-transmitted frame was observed, for the turn time.
	 *
	 * The flag is separate rather than using 0 as "nothing heard yet",
	 * because 0 is a perfectly good timestamp: `at` is elapsed samples and
	 * the first frame after a device opens can legitimately be at 0. Folding
	 * the two meanings together made the first turnaround of every session
	 * report 0 ms -- a plausible-looking number, which is the worst kind of
	 * wrong for a measurement somebody is going to tune against. */
	quint64 m_lastRxEnd = 0;
	bool m_haveRx = false;
};

#endif /* ARDOP_UI_SESSIONHISTORY_H_ */
