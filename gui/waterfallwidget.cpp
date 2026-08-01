#include "waterfallwidget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

/**
 * @file waterfallwidget.cpp
 * @brief The scrolling spectrogram (see waterfallwidget.h).
 */

namespace {
/* Rows allocated initially. The image is grown to at least the canvas height so
 * rows can be blitted one-to-one; this is only the starting size. */
constexpr int kHistory = 700;

/* Display window above the tracked noise floor. Wide enough that a weak signal
 * is visible and a strong one does not smear the whole passband. */
constexpr float kRangeDb = 45.0f;

/* Noise-floor tracking. Rises slowly and falls quickly, so a burst of traffic
 * does not drag the floor up with it and blank the display afterwards. */
constexpr float kFloorRise = 0.02f;
constexpr float kFloorFall = 0.25f;

/* Height reserved under the waterfall for the frequency axis. */
constexpr int kAxisHeight = 16;
}   // namespace

WaterfallWidget::WaterfallWidget(QWidget *parent)
	: QWidget(parent)
{
	setMinimumHeight(120);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setSpectrumGeometry(m_bins, m_firstBin, m_binHz);
}

void WaterfallWidget::setSpectrumGeometry(int bins, int firstBin, float binHz)
{
	m_bins = std::max(1, bins);
	m_firstBin = firstBin;
	m_binHz = binHz;
	m_img = QImage();
	ensureHeight(std::max(canvasHeight(), kHistory));
	m_rows = 0;
	m_haveFloor = false;
	update();
}

int WaterfallWidget::historyHeight() const
{
	return m_img.height();
}

/*
 * Keep the image at least as tall as the canvas.
 *
 * This is what makes the scroll smooth. Painting used to scale a fixed 700-row
 * image into whatever height the widget happened to be, so every new row moved
 * the picture by a fractional pixel and the resampling shifted which source row
 * landed on which output row -- the display shimmered and appeared to bounce.
 * With the image at least canvas-height, rows blit one-to-one and a new row
 * moves the picture by exactly one pixel.
 */
void WaterfallWidget::ensureHeight(int rows)
{
	if (m_img.height() >= rows && m_img.width() == m_bins)
		return;

	QImage grown(m_bins, std::max(rows, kHistory), QImage::Format_RGB32);
	grown.fill(Qt::black);
	if (!m_img.isNull() && m_img.width() == m_bins) {
		const int copy = std::min(m_img.height(), grown.height());
		for (int y = 0; y < copy; y++)
			memcpy(grown.scanLine(y), m_img.constScanLine(y),
			       size_t(std::min(grown.bytesPerLine(),
					       m_img.bytesPerLine())));
	}
	m_img = grown;
}

void WaterfallWidget::clear()
{
	m_img.fill(Qt::black);
	m_rows = 0;
	m_haveFloor = false;
	update();
}

/*
 * A perceptually monotonic ramp: black -> blue -> green -> yellow -> white.
 * Brightness rises across the whole range, so a signal reads as "brighter"
 * rather than merely "a different colour" -- which matters when the thing being
 * judged is whether a trace is above the noise.
 */
QRgb WaterfallWidget::colourFor(float t) const
{
	t = std::clamp(t, 0.0f, 1.0f);

	float r, g, b;
	if (t < 0.25f) {            /* black -> blue */
		float u = t / 0.25f;
		r = 0.0f; g = 0.0f; b = u * 0.7f;
	} else if (t < 0.5f) {      /* blue -> green */
		float u = (t - 0.25f) / 0.25f;
		r = 0.0f; g = u * 0.8f; b = 0.7f * (1.0f - u);
	} else if (t < 0.75f) {     /* green -> yellow */
		float u = (t - 0.5f) / 0.25f;
		r = u; g = 0.8f + 0.2f * u; b = 0.0f;
	} else {                    /* yellow -> white */
		float u = (t - 0.75f) / 0.25f;
		r = 1.0f; g = 1.0f; b = u;
	}
	return qRgb(int(r * 255.0f), int(g * 255.0f), int(b * 255.0f));
}

void WaterfallWidget::addRow(const SpectrumRow &row)
{
	const int n = std::min<int>(int(row.mag.size()), m_img.width());
	if (n <= 0)
		return;

	/* Power to dB once, reused for both the floor estimate and the paint.
	 * The +1 floors the log at 0 dB for an all-zero (silent) row. */
	QVector<float> db(n);
	for (int i = 0; i < n; i++)
		db[i] = 10.0f * std::log10(row.mag[i] + 1.0f);

	/* Track the floor on the row's quieter half rather than its minimum:
	 * a single dead bin would otherwise pin the scale. */
	QVector<float> sorted = db;
	std::nth_element(sorted.begin(), sorted.begin() + n / 4, sorted.end());
	float quiet = sorted[n / 4];

	if (!m_haveFloor) {
		m_floorDb = quiet;
		m_haveFloor = true;
	} else {
		float a = quiet > m_floorDb ? kFloorRise : kFloorFall;
		m_floorDb += a * (quiet - m_floorDb);
	}

	/* Scroll down by one row, then write the new row at the top. memmove
	 * over the image's own buffer is cheap at 206 x 700. */
	const int h = historyHeight();
	uchar *bits = m_img.bits();
	const qsizetype stride = m_img.bytesPerLine();
	memmove(bits + stride, bits, static_cast<size_t>(stride) * (h - 1));

	auto *top = reinterpret_cast<QRgb *>(bits);
	for (int i = 0; i < n; i++)
		top[i] = colourFor((db[i] - m_floorDb) / kRangeDb);
	for (int i = n; i < m_img.width(); i++)
		top[i] = qRgb(0, 0, 0);

	m_rows++;
	update();
}

void WaterfallWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	ensureHeight(canvasHeight());
	update();
}

int WaterfallWidget::canvasHeight() const
{
	return std::max(0, height() - kAxisHeight);
}

void WaterfallWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), Qt::black);

	const int axisH = kAxisHeight;
	QRect canvas(0, 0, width(), canvasHeight());

	/* One image row per output row: only the horizontal axis is scaled
	 * (bins to pixels), which is a constant stretch and so does not shimmer
	 * as rows arrive. */
	if (m_rows > 0 && canvas.height() > 0)
		p.drawImage(canvas, m_img,
			    QRect(0, 0, m_img.width(),
				  std::min(canvas.height(), m_img.height())));

	/* Frequency axis, from the geometry the stream announced. */
	p.setPen(QColor(160, 160, 160));
	QFont f = p.font();
	f.setPointSizeF(std::max(6.0, f.pointSizeF() - 1.5));
	p.setFont(f);

	const float loHz = m_firstBin * m_binHz;
	const float hiHz = (m_firstBin + m_bins) * m_binHz;
	for (int hz = 500; hz <= 2500; hz += 500) {
		if (hz < loHz || hz > hiHz)
			continue;
		float frac = (hz - loHz) / (hiHz - loHz);
		int x = int(frac * width());
		p.drawLine(x, canvas.bottom() - 3, x, canvas.bottom());
		p.drawText(QRect(x - 25, canvas.bottom(), 50, axisH),
			   Qt::AlignHCenter | Qt::AlignVCenter,
			   QString::number(hz));
	}

	if (m_rows == 0) {
		p.setPen(QColor(120, 120, 120));
		p.drawText(canvas, Qt::AlignCenter, tr("waiting for audio"));
	}
}
