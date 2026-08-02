#include "transcript.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QVBoxLayout>

/**
 * @file transcript.cpp
 * @brief The shared transcript (see transcript.h).
 */

Transcript::Transcript(bool monospace, QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);

	m_view = new QPlainTextEdit(this);
	m_view->setReadOnly(true);
	if (monospace)
		m_view->setFont(
			QFontDatabase::systemFont(QFontDatabase::FixedFont));

	/* Bounded like everything else here. Deep enough to hold a whole Winlink
	 * session's traffic, which is what someone will be asked to paste into a
	 * bug report. */
	m_view->setMaximumBlockCount(5000);
	root->addWidget(m_view, 1);

	auto *row = new QHBoxLayout;
	row->addStretch(1);
	m_follow = new QCheckBox(tr("Follow"), this);
	m_follow->setChecked(true);
	m_follow->setToolTip(tr("Scroll to the newest line. Turn this off to "
				"read back while traffic is still arriving."));
	row->addWidget(m_follow);
	root->addLayout(row);
}

void Transcript::setFollowVisible(bool on)
{
	m_follow->setVisible(on);
}

void Transcript::append(const QString &prefix, const QString &text,
			const char *colour)
{
	QScrollBar *bar = m_view->verticalScrollBar();
	const int wasAt = bar->value();
	const bool follow = m_follow->isChecked();

	/*
	 * Both halves are escaped, and the prefix's spaces are turned into
	 * non-breaking ones so that a two-character marker and a one-character
	 * marker still line up -- HTML collapses the padding otherwise.
	 */
	QString mark = prefix.toHtmlEscaped();
	mark.replace(QLatin1Char(' '), QLatin1String("&nbsp;"));

	/*
	 * A newline inside the text would otherwise render as a space and put
	 * two messages on one line. A peer can send them, so it is handled
	 * rather than assumed away.
	 */
	QString body = text.toHtmlEscaped();
	body.replace(QLatin1Char('\n'), QLatin1String("<br>"));

	m_view->appendHtml(
		QStringLiteral("<span style='color:#7f8c8d'>%1</span> "
			       "<span style='color:%2'>%3 %4</span>")
			.arg(QDateTime::currentDateTime().toString("HH:mm:ss"),
			     QString::fromUtf8(colour ? colour : "#b0b0b0"),
			     mark, body));

	if (follow)
		bar->setValue(bar->maximum());
	else
		bar->setValue(wasAt);
}
