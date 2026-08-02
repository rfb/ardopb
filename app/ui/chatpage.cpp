#include "chatpage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

/**
 * @file chatpage.cpp
 * @brief The chat screen (see chatpage.h).
 */

ChatPage::ChatPage(AspSession *asp, QWidget *parent)
	: QWidget(parent), m_asp(asp)
{
	auto *root = new QVBoxLayout(this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	root->addWidget(m_status);

	/* Proportional rather than fixed-pitch: this is prose from a person,
	 * where the Console is protocol text that has to line up. */
	m_transcript = new Transcript(false, this);
	root->addWidget(m_transcript, 1);

	auto *row = new QHBoxLayout;
	m_entry = new QLineEdit(this);
	m_entry->setPlaceholderText(tr("a message to the other station"));
	/*
	 * §3 puts TEXT's payload at 4096 bytes, and a line this long takes over
	 * a minute of airtime at 500 Hz. The limit is in characters and UTF-8 is
	 * up to four bytes each, so this is conservative by design -- the
	 * alternative is a message the session refuses after the operator has
	 * typed it.
	 */
	m_entry->setMaxLength(1000);
	row->addWidget(m_entry, 1);

	m_send = new QPushButton(tr("Send"), this);
	m_send->setDefault(true);
	row->addWidget(m_send);
	root->addLayout(row);

	connect(m_entry, &QLineEdit::returnPressed, this, &ChatPage::onSend);
	connect(m_send, &QPushButton::clicked, this, &ChatPage::onSend);

	updateGate();
}

void ChatPage::onStateChanged(int state, const QString &peer)
{
	m_state = state;
	if (!peer.isEmpty())
		m_peer = peer;
	if (state == ASP_LINK_IDLE) {
		m_peer.clear();
		/* Per session: the next caller deserves to be told too. */
		m_saidBinary = false;
	}
	updateGate();
}

void ChatPage::updateGate()
{
	const bool live = m_state == ASP_LINK_ASP || m_state == ASP_LINK_RAW;
	m_entry->setEnabled(live);
	m_send->setEnabled(live);

	switch (m_state) {
	case ASP_LINK_ASP:
		m_status->setText(
			tr("Connected to %1. Chat and file transfer are both "
			   "available.")
				.arg(m_peer.isEmpty() ? tr("the other station")
						      : m_peer));
		m_status->setStyleSheet("color: #2ecc71;");
		break;
	case ASP_LINK_RAW:
		/*
		 * Named for what it means to the operator rather than for what
		 * it means to the protocol. "RAW" is a state of our session;
		 * "is not running this program" is the fact behind it, and it
		 * is the one that explains why the Files screen is closed.
		 */
		m_status->setText(
			tr("Connected. The other station is not running this "
			   "program, so this is plain text in both directions "
			   "-- chat works, file transfer does not."));
		m_status->setStyleSheet("color: #d89b2e;");
		break;
	case ASP_LINK_HELLO_SENT:
		m_status->setText(
			tr("Connected. Waiting to hear what the other station "
			   "speaks -- it cannot answer until it gets a turn on "
			   "the channel."));
		m_status->setStyleSheet("color: #d89b2e;");
		break;
	default:
		m_status->setText(
			tr("No session. Connect to a station from the Station "
			   "screen, or wait for one to call."));
		m_status->setStyleSheet("color: #7f8c8d;");
		break;
	}
}

void ChatPage::onSend()
{
	const QString text = m_entry->text();
	if (text.isEmpty())
		return;

	if (!m_asp->sendText(text)) {
		/*
		 * Refused rather than queued, and said so. The session says no
		 * when there is no room for the message right now; retrying is
		 * the operator's decision because the text is still in front of
		 * them and a queue of things they have forgotten saying is
		 * worse than a line that did not go.
		 */
		m_transcript->append(QStringLiteral("!!"),
				     tr("not sent: the link would not take it "
					"just now. Try again in a moment."),
				     "#c0392b");
		return;
	}

	m_transcript->append(QStringLiteral(">>"), text, "#4fa3e8");
	m_entry->clear();
}

void ChatPage::onTextArrived(const QString &text, bool raw)
{
	/*
	 * The marker distinguishes a framed message from unframed bytes,
	 * because in raw mode there is no message boundary on the wire: what
	 * arrives is whatever the peer's end of the link chose to send, and it
	 * may be half a sentence.
	 */
	m_transcript->append(raw ? QStringLiteral(" <") : QStringLiteral("<<"),
			     text, "#2ecc71");
}

/*
 * Said once per session, not once per chunk.
 *
 * A file arriving this way is hundreds of payloads, and a line each would bury
 * the explanation in the noise it is explaining.
 */
void ChatPage::onBinaryArrived(qint64 bytes)
{
	(void)bytes;
	if (m_saidBinary)
		return;
	m_saidBinary = true;
	m_transcript->append(
		QStringLiteral("!!"),
		tr("The other station is sending data rather than text -- most "
		   "likely piping a file with ardop-cat, which has no way to "
		   "name it or check it. It cannot be received here; ask them "
		   "to send it from the Files screen instead."),
		"#c0392b");
}

void ChatPage::onNote(const QString &text)
{
	m_transcript->append(QStringLiteral("--"), text, "#7f8c8d");
}
