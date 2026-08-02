#include "consolepage.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QScrollBar>
#include <QVBoxLayout>

/**
 * @file consolepage.cpp
 * @brief The TNC command line (see consolepage.h).
 */

ConsolePage::ConsolePage(ModemThread *modem, QWidget *parent)
	: QWidget(parent), m_modem(modem)
{
	auto *root = new QVBoxLayout(this);

	m_transcript = new QPlainTextEdit(this);
	m_transcript->setReadOnly(true);
	m_transcript->setFont(
		QFontDatabase::systemFont(QFontDatabase::FixedFont));
	/* Bounded like everything else here. Deep enough to hold a whole Winlink
	 * session's command traffic, which is what someone will be asked to
	 * paste into a bug report. */
	m_transcript->setMaximumBlockCount(5000);
	root->addWidget(m_transcript, 1);

	auto *row = new QHBoxLayout;
	row->addWidget(new QLabel(tr("Command"), this));

	m_entry = new QLineEdit(this);
	m_entry->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	m_entry->setPlaceholderText(
		tr("a TNC command, e.g. MYCALL or STATE or VERSION"));
	m_entry->installEventFilter(this);   /* for Up and Down */
	row->addWidget(m_entry, 1);

	m_autoscroll = new QCheckBox(tr("Follow"), this);
	m_autoscroll->setChecked(true);
	m_autoscroll->setToolTip(
		tr("Scroll to the newest line. Turn this off to read back "
		   "while traffic is still arriving."));
	row->addWidget(m_autoscroll);

	root->addLayout(row);

	connect(m_entry, &QLineEdit::returnPressed, this, &ConsolePage::onSubmit);

	append(QStringLiteral("--"),
	       tr("Commands go to the same parser a TNC client reaches. "
		  "Anything a client can do, this can do."),
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
	append(QStringLiteral(">>"), line, "#4fa3e8");

	if (!m_modem->submitLine(line))
		append(QStringLiteral("!!"),
		       tr("not sent: the command queue is full"), "#c0392b");

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
	append(QStringLiteral("<<"), text, fault ? "#c0392b" : "#2ecc71");
}

void ConsolePage::appendMessage(const QString &text)
{
	append(QStringLiteral(" *"), text, "#d89b2e");
}

void ConsolePage::append(const QString &prefix, const QString &text,
			 const char *colour)
{
	/*
	 * Preserving the scroll position when Follow is off is the whole reason
	 * this is not three calls to appendPlainText: a transcript that yanks
	 * itself to the bottom while someone is reading back through it is one
	 * they cannot read back through.
	 */
	QScrollBar *bar = m_transcript->verticalScrollBar();
	const int wasAt = bar->value();
	const bool follow = m_autoscroll->isChecked();

	/*
	 * The prefix is escaped as carefully as the text.
	 *
	 * "<<" is a perfectly good direction marker and a perfectly good start
	 * of an HTML tag, and appendHtml believes the second reading: every
	 * received line rendered as a bare timestamp with its content swallowed.
	 * Leading spaces collapse too, so the marker is padded with &nbsp;.
	 */
	QString mark = prefix.toHtmlEscaped();
	mark.replace(QLatin1Char(' '), QLatin1String("&nbsp;"));

	m_transcript->appendHtml(
		QStringLiteral("<span style='color:#7f8c8d'>%1</span> "
			       "<span style='color:%2'>%3 %4</span>")
			.arg(QDateTime::currentDateTime().toString("HH:mm:ss"),
			     QString::fromUtf8(colour), mark,
			     text.toHtmlEscaped()));

	if (follow)
		bar->setValue(bar->maximum());
	else
		bar->setValue(wasAt);
}
