#ifndef ARDOP_UI_CHATPAGE_H_
#define ARDOP_UI_CHATPAGE_H_

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

#include "aspsession.h"
#include "transcript.h"

/**
 * @file chatpage.h
 * @brief Talking to the station at the other end.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §3 asks for a message list
 * and a compose box; [17](../../analysis/17-application-protocol.md) §6 supplies
 * the message. Between them there is very little left to design, which is the
 * point -- the protocol carries no per-message timestamp or callsign because the
 * session already establishes both, so this screen is a transcript and a line
 * edit.
 *
 * ## Raw mode is a first-class state, not a degraded one
 *
 * §2: a peer that does not open with a well-formed HELLO is not speaking ASP, and
 * the session degrades to unframed UTF-8 in both directions. **Most stations on
 * the air are running plain ardopcf or a terminal**, and rag-chewing with them
 * correctly is a feature rather than a fallback grudgingly tolerated.
 *
 * So this screen says which one it is in plain words, and the difference an
 * operator can act on -- files need ASP, chat does not -- is stated where they
 * would otherwise go looking for a broken button.
 *
 * ## Nothing here is typed into the air by accident
 *
 * The compose box is disabled unless a session exists. That is not politeness
 * about greyed-out controls: with no session the bytes would sit in the transmit
 * queue and go out at the start of the *next* connection, to whoever that turns
 * out to be.
 */
class ChatPage : public QWidget {
	Q_OBJECT

public:
	explicit ChatPage(AspSession *asp, QWidget *parent = nullptr);

public slots:
	void onStateChanged(int state, const QString &peer);
	void onTextArrived(const QString &text, bool raw);

	/** @brief The peer is piping a file at us; say so, once. */
	void onBinaryArrived(qint64 bytes);
	void onNote(const QString &text);

private slots:
	void onSend();

private:
	void updateGate();

	AspSession *m_asp = nullptr;
	Transcript *m_transcript = nullptr;
	QLineEdit *m_entry = nullptr;
	QPushButton *m_send = nullptr;
	QLabel *m_status = nullptr;

	int m_state = 0;   /**< ::asp_link_state. */
	bool m_saidBinary = false;
	QString m_peer;
};

#endif /* ARDOP_UI_CHATPAGE_H_ */
