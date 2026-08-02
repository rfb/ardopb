#ifndef ARDOP_UI_HISTORYPAGE_H_
#define ARDOP_UI_HISTORYPAGE_H_

#include <QAbstractTableModel>
#include <QLabel>
#include <QTableView>
#include <QWidget>

#include "historygrid.h"
#include "sessionhistory.h"

/**
 * @file historypage.h
 * @brief The session history, at both resolutions, sharing one cursor.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §10. The grid above shows
 * the shape of the whole session; the table below shows every detail of one
 * frame. Clicking either moves both, which is what makes the pair worth more
 * than either alone: the grid is where a problem is *noticed* and the table is
 * where it is *identified*, and without a shared cursor an operator has to find
 * the same moment twice.
 */

/**
 * @brief The table's view of the same ring. Not a copy.
 *
 * Every column is derived from the frame type through `core/codec/frame.h`, so
 * the mode names and payload sizes come from the protocol and not from a table
 * here -- §16's exit criterion, and the reason adding a data mode to `core/`
 * needs no change in this file.
 */
class HistoryModel : public QAbstractTableModel {
	Q_OBJECT

public:
	explicit HistoryModel(SessionHistory *history, QObject *parent = nullptr);

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	int columnCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QVariant headerData(int section, Qt::Orientation o,
			    int role) const override;

private:
	SessionHistory *m_history = nullptr;
};

class HistoryPage : public QWidget {
	Q_OBJECT

public:
	explicit HistoryPage(SessionHistory *history, QWidget *parent = nullptr);

private slots:
	/** @brief Move both views. The guard here stops the two looping. */
	void selectFrame(int index);
	void onAppended();

private:
	SessionHistory *m_history = nullptr;
	HistoryGrid *m_grid = nullptr;
	HistoryModel *m_model = nullptr;
	QTableView *m_table = nullptr;
	QLabel *m_summary = nullptr;
	int m_cursor = -1;

	/* Whether the table should follow new arrivals. Turned off the moment
	 * the operator selects something, because scrolling away from what
	 * somebody is reading is the fastest way to make a log unreadable. */
	bool m_follow = true;
};

#endif /* ARDOP_UI_HISTORYPAGE_H_ */
