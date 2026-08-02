#include "levelmeter.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

/**
 * @file levelmeter.cpp
 * @brief The horizontal level meter (see levelmeter.h).
 */

namespace {

/* Ballistics. The level's decay is the VU integration time -- slow enough that
 * the bar is readable rather than flickering, fast enough to follow speech. The
 * peak's hold and fall are the broadcast PPM convention: long enough to read,
 * then a straight fall in dB per second rather than an exponential one, so the
 * rate a peak leaves at does not depend on how high it was. */
constexpr double kLevelDecaySec = 0.300;
constexpr int kPeakHoldMs = 1500;
constexpr double kPeakFallPerSec = 20.0;

/*
 * Layout, in logical pixels.
 *
 * kTickBand has to hold a line of text *and* the 3 px tick beneath it. At 10 it
 * held neither: the scale numbers were drawn into a 7 px box and came out cut in
 * half, which is the sort of thing that is obvious in a screenshot and invisible
 * in the source.
 */
constexpr int kTickLine = 3;
constexpr int kTickBand = 15;    /* text + kTickLine */
constexpr int kTroughBand = 15;
constexpr int kTextBand = 13;

/* The bar, keyed off the band rather than a single threshold. */
const QColor kTooQuiet(0x50, 0x60, 0x70);   /* present, but far too low */
const QColor kInBand(0x2e, 0xa0, 0x4e);
const QColor kHot(0xd0, 0x92, 0x1c);        /* headroom going */
const QColor kClipping(0xc0, 0x39, 0x2b);

const QColor kTrough(0x1a, 0x1d, 0x21);
const QColor kBandFill(0x2e, 0xa0, 0x4e, 40);
const QColor kPeakMark(0xec, 0xf0, 0xf1);

}   // namespace

LevelMeter::LevelMeter(const QString &label, QWidget *parent)
	: QWidget(parent), m_label(label)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	m_clock.start();

	/* Independent of the data rate, which is the whole point -- see the
	 * header. Thirty per second is the panel's rate everywhere else. */
	connect(&m_repaint, &QTimer::timeout, this, &LevelMeter::tick);
	m_repaint.start(33);
}

void LevelMeter::setScale(double min, double max)
{
	if (max <= min)
		return;
	m_min = min;
	m_max = max;
	update();
}

void LevelMeter::setBand(double lo, double hi)
{
	m_bandLo = lo;
	m_bandHi = hi;
	update();
}

void LevelMeter::setValue(double level, double peak)
{
	/*
	 * Only the raw figures. Nothing decays here: this is called once per
	 * captured audio block, at whatever rate the backend chose, and putting
	 * the ballistics on that clock is the bug this design avoids.
	 */
	m_rawLevel = level;
	m_rawPeak = std::max(peak, level);

	if (!m_haveData) {
		/* Attack instantly into the first reading rather than sliding up
		 * from the sentinel, which would look like a signal fading in. */
		m_level = m_rawLevel;
		m_peak = m_rawPeak;
		m_peakHoldUntilMs = m_clock.elapsed() + kPeakHoldMs;
		m_haveData = true;
	}
}

void LevelMeter::setUnknown(const QString &caption)
{
	m_haveData = false;
	m_caption = caption;
	m_level = m_peak = m_rawLevel = m_rawPeak = -1e9;
	update();
}

void LevelMeter::setCaption(const QString &caption)
{
	m_caption = caption;
}

/* The ballistics themselves, factored out so the test can drive them with a
 * known dt instead of waiting on a wall clock. */
void LevelMeter::step(double dt)
{
	if (!m_haveData)
		return;

	/*
	 * Level: instant attack, exponential decay.
	 *
	 * The decay is written as exp(-dt/tau) rather than as the linear
	 * `+= (raw - level) * min(1, dt/tau)` that §9 specified, because only
	 * this form is actually independent of the step size. The linear one
	 * agrees for small dt and diverges badly for large: half a second
	 * delivered as one step clamps the factor to 1 and snaps straight to the
	 * new value, where the same half second in ten steps lands 6 dB short.
	 *
	 * That matters because the step size here is exactly what is not under
	 * our control -- a repaint can be late, and a machine under load can
	 * deliver one 500 ms tick instead of fifteen. The whole reason the
	 * ballistics are on a clock is so the meter behaves the same either way,
	 * and the linear form quietly gave that back.
	 */
	if (m_rawLevel >= m_level)
		m_level = m_rawLevel;
	else
		m_level = m_rawLevel +
			  (m_level - m_rawLevel) * std::exp(-dt / kLevelDecaySec);

	/* Peak: instant attack, hold, then a straight fall. */
	const qint64 now = m_clock.elapsed();
	if (m_rawPeak >= m_peak) {
		m_peak = m_rawPeak;
		m_peakHoldUntilMs = now + kPeakHoldMs;
	} else if (now > m_peakHoldUntilMs) {
		m_peak -= kPeakFallPerSec * dt;
		m_peak = std::max(m_peak, m_level);   /* never below the bar */
	}
}

void LevelMeter::tick()
{
	const qint64 now = m_clock.elapsed();
	const double dt = double(now - m_lastTickMs) / 1000.0;
	m_lastTickMs = now;

	if (!m_haveData)
		return;

	step(dt);
	update();
}

void LevelMeter::advanceForTest(qint64 ms)
{
	m_peakHoldUntilMs -= ms;   /* as if that much wall time had passed */
	step(double(ms) / 1000.0);
	update();
}

int LevelMeter::toX(double v) const
{
	const double t = std::clamp((v - m_min) / (m_max - m_min), 0.0, 1.0);
	return int(std::lround(t * (width() - 1)));
}

QColor LevelMeter::barColour() const
{
	/*
	 * Four bands, not two. A single threshold would colour a signal 40 dB
	 * too quiet the same as a correct one, which is the failure this widget
	 * was built to stop reassuring people about.
	 */
	if (m_level >= m_max - 1.0)
		return kClipping;
	if (m_level > m_bandHi)
		return kHot;
	if (m_level >= m_bandLo)
		return kInBand;
	return kTooQuiet;
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	const int troughTop = kTickBand;
	const int troughH =
		std::min(kTroughBand, height() - kTickBand - kTextBand);
	const QRect trough(0, troughTop, width(), std::max(4, troughH));

	QFont f = p.font();
	f.setPointSizeF(std::max(6.5, f.pointSizeF() - 2.0));
	p.setFont(f);

	/* --- ticks, above the trough ---------------------------------------- */
	p.setPen(QColor(0x7f, 0x8c, 0x8d));
	for (double v : {m_min, m_bandLo, m_bandHi, m_max}) {
		const int x = toX(v);
		p.drawLine(x, troughTop - kTickLine, x, troughTop);
		const QString s = QString::number(int(std::lround(v)));
		const int w = QFontMetrics(f).horizontalAdvance(s);
		/* Clamped so the end labels stay inside the widget rather than
		 * being clipped in half by its own edge. */
		const int tx = std::clamp(x - w / 2, 0, width() - w);
		p.drawText(QRect(tx, 0, w, troughTop - kTickLine),
			   Qt::AlignHCenter | Qt::AlignVCenter, s);
	}

	/* --- the trough, in three passes ------------------------------------ */
	p.fillRect(trough, kTrough);

	/* The band first, so it stays visible where the bar has not reached it.
	 * This is what makes "too quiet" legible: the gap between the bar's end
	 * and the band's start is the thing to look at. */
	const int bandX0 = toX(m_bandLo), bandX1 = toX(m_bandHi);
	p.fillRect(QRect(bandX0, trough.top(), std::max(1, bandX1 - bandX0),
			 trough.height()),
		   kBandFill);

	if (m_haveData) {
		const int barX = toX(m_level);
		p.fillRect(QRect(0, trough.top(), barX, trough.height()),
			   barColour());

		/* The peak, as a line rather than a fill: it marks where the
		 * signal has been, and a filled region would read as where it
		 * is. */
		if (m_peak > m_level) {
			const int px = toX(m_peak);
			p.fillRect(QRect(std::max(0, px - 1), trough.top(), 2,
					 trough.height()),
				   kPeakMark);
		}
	}

	p.setPen(QColor(0x44, 0x4a, 0x50));
	p.drawRect(trough.adjusted(0, 0, -1, -1));

	/* --- label and reading, below --------------------------------------- */
	const int textTop = trough.bottom() + 1;
	const int textH = std::max(0, height() - textTop);
	if (textH >= 8) {
		p.setPen(palette().color(QPalette::WindowText));
		p.drawText(QRect(0, textTop, width() / 2, textH),
			   Qt::AlignLeft | Qt::AlignVCenter, m_label);
		p.setPen(m_haveData ? palette().color(QPalette::WindowText)
				    : QColor(0x7f, 0x8c, 0x8d));
		p.drawText(QRect(width() / 2, textTop, width() / 2 - 1, textH),
			   Qt::AlignRight | Qt::AlignVCenter, m_caption);
	}
}
