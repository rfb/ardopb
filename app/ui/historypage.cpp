#include "historypage.h"

#include <QHeaderView>
#include <QVBoxLayout>

extern "C" {
#include "codec/frame.h"
}

/**
 * @file historypage.cpp
 * @brief The two views and the cursor they share (see historypage.h).
 */

namespace {

constexpr quint64 kSampleRate = 12000;

enum Column { ColAt = 0, ColDir, ColMode, ColBytes, ColQ, ColSn, ColTurn,
	      ColCount };

QString direction(quint8 dir)
{
	switch (dir) {
	case ARDOP_TLM_DIR_TX:        return QStringLiteral("TX");
	case ARDOP_TLM_DIR_RX_FAILED: return QStringLiteral("RX bad");
	default:                      return QStringLiteral("RX");
	}
}

QString elapsed(quint64 samples)
{
	const double sec = double(samples) / double(kSampleRate);
	const int m = int(sec) / 60;
	return QStringLiteral("%1:%2")
		.arg(m, 2, 10, QLatin1Char('0'))
		.arg(sec - m * 60, 4, 'f', 1, QLatin1Char('0'));
}

}   // namespace

HistoryModel::HistoryModel(SessionHistory *history, QObject *parent)
	: QAbstractTableModel(parent), m_history(history)
{
	/* The ring tells the model what happened to it, and the model turns that
	 * into the begin/end calls a view needs. Nothing is copied. */
	connect(history, &SessionHistory::appended, this, [this](int index) {
		beginInsertRows(QModelIndex(), index, index);
		endInsertRows();
	});
	connect(history, &SessionHistory::wrapped, this, [this] {
		beginRemoveRows(QModelIndex(), 0, 0);
		endRemoveRows();
	});
	connect(history, &SessionHistory::cleared, this, [this] {
		beginResetModel();
		endResetModel();
	});
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : m_history->count();
}

int HistoryModel::columnCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : ColCount;
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() >= m_history->count())
		return QVariant();

	const SessionHistory::Record &r = m_history->at(index.row());

	if (role == Qt::TextAlignmentRole && index.column() != ColMode)
		return int(Qt::AlignRight | Qt::AlignVCenter);

	if (role != Qt::DisplayRole)
		return QVariant();

	switch (index.column()) {
	case ColAt:
		return elapsed(r.at);
	case ColDir:
		return direction(r.dir);
	case ColMode: {
		/* From core, never from a table here. */
		char name[32];
		if (ardop_data_frame_name(r.frameType, name, sizeof name))
			return QString::fromUtf8(name);
		const ardop_frame_spec *spec = ardop_frame_spec_for(r.frameType);
		return spec ? QString::fromUtf8(spec->name)
			    : QStringLiteral("0x%1").arg(r.frameType, 2, 16,
							 QLatin1Char('0'));
	}
	case ColBytes: {
		const ardop_frame_spec *spec = ardop_frame_spec_for(r.frameType);
		const uint16_t n = spec ? ardop_frame_payload_bytes(spec) : 0;
		/* Blank rather than zero for a control frame: it does not carry
		 * a payload, which is different from carrying an empty one. */
		return n ? QVariant(n) : QVariant();
	}
	case ColQ:
		return r.quality >= 0 ? QVariant(r.quality) : QVariant();
	case ColSn:
		return r.sn >= 0 ? QVariant(r.sn) : QVariant();
	case ColTurn:
		return r.turnMs ? QVariant(QStringLiteral("%1 ms").arg(r.turnMs))
				: QVariant();
	}
	return QVariant();
}

QVariant HistoryModel::headerData(int section, Qt::Orientation o, int role) const
{
	if (role != Qt::DisplayRole || o != Qt::Horizontal)
		return QVariant();
	switch (section) {
	case ColAt:    return tr("at");
	case ColDir:   return tr("dir");
	case ColMode:  return tr("mode");
	case ColBytes: return tr("bytes");
	case ColQ:     return tr("Q");
	case ColSn:    return tr("S/N");
	case ColTurn:  return tr("turn");
	}
	return QVariant();
}

/* --------------------------------------------------------------------------- */

HistoryPage::HistoryPage(SessionHistory *history, QWidget *parent)
	: QWidget(parent), m_history(history)
{
	auto *root = new QVBoxLayout(this);

	m_grid = new HistoryGrid(history, this);
	root->addWidget(m_grid, 0);

	/* Seeded from the ring rather than hardcoded, because a page built after
	 * frames have already arrived would otherwise open claiming there are
	 * none. */
	m_summary = new QLabel(this);
	root->addWidget(m_summary, 0);

	m_model = new HistoryModel(history, this);
	m_table = new QTableView(this);
	m_table->setModel(m_model);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->verticalHeader()->setVisible(false);
	/* Ten thousand rows is enough for the per-row layout cost to show. */
	m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	m_table->verticalHeader()->setDefaultSectionSize(18);
	m_table->horizontalHeader()->setStretchLastSection(true);
	root->addWidget(m_table, 1);

	connect(m_grid, &HistoryGrid::frameClicked,
		this, &HistoryPage::selectFrame);
	connect(m_table->selectionModel(),
		&QItemSelectionModel::currentRowChanged, this,
		[this](const QModelIndex &i) {
			if (i.isValid())
				selectFrame(i.row());
		});
	connect(history, &SessionHistory::appended,
		this, &HistoryPage::onAppended);
	connect(history, &SessionHistory::cleared, this, [this] {
		m_cursor = -1;
		m_follow = true;
		m_summary->setText(tr("no frames yet"));
	});

	/* Last, once every widget it touches exists: a page built after frames
	 * have already arrived would otherwise open claiming there are none. */
	onAppended();
}

void HistoryPage::onAppended()
{
	const int n = m_history->count();
	m_summary->setText(tr("%n frame(s)", nullptr, n));

	/* Follow the newest only while nobody is reading back. */
	if (m_follow && n > 0)
		m_table->scrollToBottom();
}

void HistoryPage::selectFrame(int index)
{
	/*
	 * The guard that stops the two selection signals looping.
	 *
	 * The grid's click sets the table's row, which emits currentRowChanged,
	 * which comes back here. Without the early return that is an infinite
	 * round trip -- and it would not crash, it would just wedge, which is
	 * the harder kind to find.
	 */
	if (index == m_cursor)
		return;

	m_cursor = index;
	m_follow = false;   /* the operator is reading, not watching */

	m_grid->setCursorIndex(index);
	if (index >= 0 && index < m_model->rowCount()) {
		m_table->selectRow(index);
		m_table->scrollTo(m_model->index(index, 0),
				  QAbstractItemView::PositionAtCenter);
	}
}
