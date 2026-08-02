#include "historygrid.h"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

/**
 * @file historygrid.cpp
 * @brief The macro view (see historygrid.h).
 */

namespace {

constexpr int kCellMax = 9;
constexpr int kCellMin = 3;   /* below this a cell is not a cell */
constexpr int kGap = 1;

/* How much of the screen the macro view may take before cells start shrinking
 * instead. The table below it is the more useful view once a session is long. */
constexpr int kMaxHeight = 150;

const QColor kRxDim(0x1d, 0x3a, 0x5c);    /* received, poor quality */
const QColor kRxBright(0x4f, 0xa3, 0xe8); /* received, good quality */
const QColor kTx(0xd8, 0x9b, 0x2e);
const QColor kFailed(0xc0, 0x39, 0x2b);
const QColor kBackground(0x14, 0x16, 0x18);

QColor lerp(const QColor &a, const QColor &b, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	return QColor(int(a.red() + (b.red() - a.red()) * t),
		      int(a.green() + (b.green() - a.green()) * t),
		      int(a.blue() + (b.blue() - a.blue()) * t));
}

}   // namespace

HistoryGrid::HistoryGrid(SessionHistory *history, QWidget *parent)
	: QWidget(parent), m_history(history)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(history, &SessionHistory::appended, this, [this] {
		/* The grid gets taller as the session grows, until it hits its
		 * cap. Cheap: appends arrive a few times a second, not per
		 * sample. */
		updateGeometry();
		update();
	});
	connect(history, &SessionHistory::cleared, this, [this] {
		m_cursor = -1;
		updateGeometry();
		update();
	});
	connect(history, &SessionHistory::wrapped, this, [this] {
		/* Every index shifted down one; the cursor follows or is lost. */
		if (m_cursor >= 0)
			m_cursor--;
		update();
	});
}

/*
 * Shrink the cell until the whole history fits in kMaxHeight.
 *
 * This is what "no scrolling" costs, and it is worth it: a view that scrolls
 * shows a window onto the session, and the thing worth seeing is the shape of
 * the whole session. Below kCellMin it stops shrinking and the oldest cells fall
 * off the top instead, because an unreadable grid is worse than a partial one.
 *
 * Measured against kMaxHeight rather than against height(), so that the answer
 * does not depend on the size the layout has already given us -- which is itself
 * derived from this. sizeHint() closes the loop in one direction only.
 */
int HistoryGrid::cellSize() const
{
	const int n = m_history->count();
	if (n <= 0)
		return kCellMax;

	for (int cell = kCellMax; cell > kCellMin; cell--) {
		const int perRow = std::max(1, width() / (cell + kGap));
		const int rows = (n + perRow - 1) / perRow;
		if (rows * (cell + kGap) <= kMaxHeight)
			return cell;
	}
	return kCellMin;
}

/*
 * How many rows fit across the current width decides how tall this wants to be,
 * so a width change has to re-ask.
 *
 * The layout asks for a size hint before it has given the widget a width, and at
 * width 0 the answer is always "as tall as you will allow" -- which is how this
 * first shipped drawing three rows of cells into a 150 px black rectangle. Only
 * the width is tracked, so the height this returns cannot re-trigger the check
 * and the loop terminates after one pass.
 */
void HistoryGrid::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	if (width() != m_lastWidth) {
		m_lastWidth = width();
		updateGeometry();
	}
}

QSize HistoryGrid::sizeHint() const
{
	const int n = m_history->count();
	if (n <= 0)
		return QSize(200, 24);

	const int cell = cellSize();
	const int perRow = std::max(1, width() / (cell + kGap));
	const int rows = (n + perRow - 1) / perRow;
	return QSize(200, std::clamp(rows * (cell + kGap), 24, kMaxHeight));
}

QRect HistoryGrid::cellRect(int index, int cell, int perRow) const
{
	const int row = index / perRow;
	const int col = index % perRow;
	return QRect(col * (cell + kGap), row * (cell + kGap), cell, cell);
}

int HistoryGrid::indexAt(const QPoint &p) const
{
	const int cell = cellSize();
	const int perRow = std::max(1, width() / (cell + kGap));
	const int col = p.x() / (cell + kGap);
	const int row = p.y() / (cell + kGap);
	if (col < 0 || col >= perRow || row < 0)
		return -1;
	const int i = row * perRow + col;
	return (i >= 0 && i < m_history->count()) ? i : -1;
}

void HistoryGrid::mousePressEvent(QMouseEvent *e)
{
	const int i = indexAt(e->pos());
	if (i >= 0)
		emit frameClicked(i);
}

void HistoryGrid::setCursorIndex(int index)
{
	if (index == m_cursor)
		return;
	m_cursor = index;
	update();
}

void HistoryGrid::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), kBackground);

	const int n = m_history->count();
	if (n == 0) {
		p.setPen(QColor(0x7f, 0x8c, 0x8d));
		p.drawText(rect(), Qt::AlignCenter, tr("no frames yet"));
		return;
	}

	const int cell = cellSize();
	const int perRow = std::max(1, width() / (cell + kGap));
	const int visibleRows = std::max(1, height() / (cell + kGap));
	const int capacity = perRow * visibleRows;

	/* When even the smallest cell will not fit the whole history, show the
	 * newest. The oldest is what an operator has already seen. */
	const int first = std::max(0, n - capacity);

	p.setRenderHint(QPainter::Antialiasing, false);

	for (int i = first; i < n; i++) {
		const SessionHistory::Record &r = m_history->at(i);
		const QRect cr = cellRect(i - first, cell, perRow);
		if (cr.top() > height())
			break;

		QColor fill;
		switch (r.dir) {
		case ARDOP_TLM_DIR_TX:
			fill = kTx;   /* no quality exists for a transmission */
			break;
		case ARDOP_TLM_DIR_RX_FAILED:
			fill = kFailed;
			break;
		default:
			fill = lerp(kRxDim, kRxBright,
				    std::clamp(r.quality / 100.0, 0.0, 1.0));
			break;
		}
		p.fillRect(cr, fill);

		/* The shape that keeps failure off hue alone. */
		if (r.dir == ARDOP_TLM_DIR_RX_FAILED && cell >= 5) {
			p.setPen(QPen(QColor(0xff, 0xff, 0xff, 180), 1));
			p.drawLine(cr.topLeft(), cr.bottomRight());
		}

		if (i == m_cursor) {
			p.setPen(QPen(palette().color(QPalette::Highlight), 1));
			p.setBrush(Qt::NoBrush);
			p.drawRect(cr.adjusted(0, 0, -1, -1));
		}
	}
}
