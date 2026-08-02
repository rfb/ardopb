#include "consolepage.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>

/**
 * @file consolepage.cpp
 * @brief The TNC command line (see consolepage.h).
 */

ConsolePage::ConsolePage(ModemThread *modem, QWidget *parent)
	: QWidget(parent), m_modem(modem)
{
	auto *root = new QVBoxLayout(this);

	m_transcript = new Transcript(true, this);
	root->addWidget(m_transcript, 1);

	auto *row = new QHBoxLayout;
	row->addWidget(new QLabel(tr("Command"), this));

	m_entry = new QLineEdit(this);
	m_entry->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	m_entry->setPlaceholderText(
		tr("a TNC command, e.g. MYCALL or STATE or VERSION"));
	m_entry->installEventFilter(this);   /* for Up and Down */
	row->addWidget(m_entry, 1);

	root->addLayout(row);

	connect(m_entry, &QLineEdit::returnPressed, this, &ConsolePage::onSubmit);

	m_transcript->append(QStringLiteral("--"),
			     tr("Commands go to the same parser a TNC client "
				"reaches. Anything a client can do, this can do."),
			     "#7f8c8d");
}

/*
 * Up and Down walk the history.
 *
 * An event filter rather than a QLineEdit subclass: it is two keys, and a whole
 * class for two keys is the kind of thing that later grows a third reason to
 * exist.
 */
bool ConsolePage::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != m_entry || event->type() != QEvent::KeyPress)
		return QWidget::eventFilter(watched, event);

	auto *key = static_cast<QKeyEvent *>(event);
	if (key->key() == Qt::Key_Up) {
		if (m_historyPos > 0)
			m_entry->setText(m_history.at(--m_historyPos));
		return true;
	}
	if (key->key() == Qt::Key_Down) {
		if (m_historyPos < m_history.size() - 1)
			m_entry->setText(m_history.at(++m_historyPos));
		else {
			m_historyPos = m_history.size();
			m_entry->clear();
		}
		return true;
	}
	return QWidget::eventFilter(watched, event);
}

void ConsolePage::onSubmit()
{
	const QString line = m_entry->text().trimmed();
	if (line.isEmpty())
		return;

	/* Echoed before it is sent, and echoed even if the queue refuses it, so
	 * the transcript is a record of what the operator asked for and not only
	 * of what got through. */
	m_transcript->append(QStringLiteral(">>"), line, "#4fa3e8");

	if (!m_modem->submitLine(line))
		m_transcript->append(QStringLiteral("!!"),
				     tr("not sent: the command queue is full"),
				     "#c0392b");

	if (m_history.isEmpty() || m_history.last() != line)
		m_history << line;
	m_historyPos = m_history.size();
	m_entry->clear();
}

void ConsolePage::appendReply(const QString &text)
{
	/* A FAULT is a reply like any other and belongs in sequence; it is
	 * coloured because it is the line someone is looking for. */
	const bool fault = text.startsWith(QLatin1String("FAULT"));
	m_transcript->append(QStringLiteral("<<"), text,
			     fault ? "#c0392b" : "#2ecc71");
}

void ConsolePage::appendMessage(const QString &text)
{
	m_transcript->append(QStringLiteral(" *"), text, "#d89b2e");
}
