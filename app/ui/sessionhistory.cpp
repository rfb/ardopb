#include "sessionhistory.h"

/**
 * @file sessionhistory.cpp
 * @brief The frame ring (see sessionhistory.h).
 */

namespace {
/* The modem's sample rate. Frame timestamps are elapsed samples, and this is the
 * only place they become milliseconds. */
constexpr quint64 kSampleRate = 12000;
}   // namespace

SessionHistory::SessionHistory(QObject *parent) : QObject(parent)
{
	/* Allocated once, at full size, and never resized: an append must not be
	 * able to reallocate under a view that is mid-paint. */
	m_ring.resize(kCapacity);
}

void SessionHistory::append(const ardop_telemetry &rec)
{
	Record r;
	r.at = rec.frame_at;
	r.frameType = rec.frame_type;
	r.dir = rec.frame_dir;
	r.quality = rec.quality;
	r.sn = rec.sn;

	/*
	 * Turn time, computed once, here.
	 *
	 * A frame's `at` is when it was *observed*, so this is the gap between
	 * one frame being decoded and the next transmission starting. Only
	 * measured when something was actually heard first -- otherwise the
	 * first transmission of a session would report a turnaround measured
	 * from the epoch.
	 */
	if (rec.frame_dir == ARDOP_TLM_DIR_TX) {
		if (m_haveRx && r.at > m_lastRxEnd)
			r.turnMs = quint32((r.at - m_lastRxEnd) * 1000 /
					   kSampleRate);
		/*
		 * Consumed, so only the *first* transmission after a reception
		 * is a turnaround. The second and third frames of the same
		 * transmission are a continuation of it; measuring them from the
		 * same reception would report the turnaround again, larger each
		 * time, and a column of numbers that all look like turn times
		 * but mean different things is worse than a blank.
		 */
		m_haveRx = false;
	} else {
		m_lastRxEnd = r.at;
		m_haveRx = true;
	}

	const bool willWrap = (m_count == kCapacity);
	m_ring[m_head] = r;
	m_head = (m_head + 1) % kCapacity;
	if (willWrap)
		emit wrapped();   /* before appended: row 0 goes, then one arrives */
	else
		m_count++;

	emit appended(m_count - 1);
}

const SessionHistory::Record &SessionHistory::at(int i) const
{
	static const Record empty;
	if (i < 0 || i >= m_count)
		return empty;

	/* Index 0 is the oldest still held. When the ring is full the oldest
	 * sits at m_head; before that it sits at 0. */
	const int base = (m_count == kCapacity) ? m_head : 0;
	return m_ring[(base + i) % kCapacity];
}

void SessionHistory::clear()
{
	m_head = 0;
	m_count = 0;
	m_lastRxEnd = 0;
	m_haveRx = false;
	emit cleared();
}
