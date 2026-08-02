#ifndef ARDOP_UI_HISTORYGRID_H_
#define ARDOP_UI_HISTORYGRID_H_

#include <QWidget>

#include "sessionhistory.h"

/**
 * @file historygrid.h
 * @brief The whole session at a glance: one cell per frame.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §10's macro view. **No
 * scrolling** -- the entire history is always visible, and the cell size falls
 * as it fills. That is the point of this view: the pattern is readable long
 * before any individual cell is, and the pattern is what tells you a session
 * spent four minutes retrying.
 *
 * ## Colour carries two dimensions without them competing
 *
 * Direction is hue -- blue received, amber transmitted -- and quality rides
 * *lightness* within the received hue. Deliberately not red-against-green:
 * that pair is the one a substantial fraction of operators cannot separate, and
 * this is a display about diagnosing faults.
 *
 * Failure is the exception and gets a *shape*: a diagonal across the cell, so it
 * is distinguishable without relying on hue at all.
 *
 * ## Dead air is drawn as dead air
 *
 * A gap between frames leaves a blank proportional to `log(gap)`, so a two
 * second turnaround stays visible without a thirty second one swallowing the
 * row. A grid with the silence squeezed out would make a session that struggled
 * look identical to one that did not.
 */
class HistoryGrid : public QWidget {
	Q_OBJECT

public:
	explicit HistoryGrid(SessionHistory *history, QWidget *parent = nullptr);

	/** @brief The record under @p p, or -1. */
	int indexAt(const QPoint &p) const;

	/**
	 * @brief As tall as the session needs, and no taller.
	 *
	 * The grid grows downward as the history fills and stops at a cap, after
	 * which the cells shrink instead. A fixed height would leave a young
	 * session drawing three rows of cells into a large black rectangle,
	 * which reads as something being broken rather than as a short session.
	 */
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return QSize(200, 24); }

signals:
	/** @brief The operator clicked a cell. */
	void frameClicked(int index);

public slots:
	/** @brief Highlight @p index. Driven by the table, via the page. */
	void setCursorIndex(int index);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	/* Where each cell lands. Recomputed on paint and on hit test from the
	 * same function, so a click can never disagree with what was drawn. */
	QRect cellRect(int index, int cell, int perRow) const;
	int cellSize() const;

	SessionHistory *m_history = nullptr;
	int m_cursor = -1;
	int m_lastWidth = -1;   /* so a width change re-asks for a height */
};

#endif /* ARDOP_UI_HISTORYGRID_H_ */
