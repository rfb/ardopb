#ifndef ARDOP_UI_CONSOLEPAGE_H_
#define ARDOP_UI_CONSOLEPAGE_H_

#include <QCheckBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringList>
#include <QWidget>

#include "modemthread.h"

/**
 * @file consolepage.h
 * @brief The TNC command line, and everything the modem said back.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §3: *"The Console is not a
 * debug afterthought -- it is how anyone diagnoses a Pat interaction, and the
 * parser is already linked, so it costs a text field."*
 *
 * Both halves of that are load-bearing. The **cost** is genuinely a text field:
 * ::app_submit_line hands a string to the same parser a TCP client reaches, so
 * there is no second command path to write or to keep in step. The **value** is
 * that when someone reports that Winlink or Pat did not work, this is the only
 * screen that shows what was actually exchanged -- the other screens show the
 * modem's state, which is a summary written by the thing under suspicion.
 *
 * ## It is a transcript, not a log
 *
 * Sent lines and received lines are both here, in order, marked by direction.
 * A console that showed only replies would lose the question each reply answers,
 * which is the half that usually contains the mistake.
 *
 * ## It can do anything, and that is the point
 *
 * Every other screen constrains what an operator can express -- typed commands,
 * validated fields, a gated Connect button. This one does not, deliberately: it
 * is the escape hatch for a radio, a peer or a client that needs something the
 * interface does not offer yet. So it accepts a malformed line and shows the
 * `FAULT` that comes back, rather than refusing to send it.
 */
class ConsolePage : public QWidget {
	Q_OBJECT

public:
	explicit ConsolePage(ModemThread *modem, QWidget *parent = nullptr);

public slots:
	/** @brief A reply to a command, from the event queue. */
	void appendReply(const QString &text);

	/** @brief An unsolicited line -- state changes, faults, guest activity. */
	void appendMessage(const QString &text);

private slots:
	void onSubmit();

private:
	void append(const QString &prefix, const QString &text, const char *colour);

	ModemThread *m_modem = nullptr;
	QPlainTextEdit *m_transcript = nullptr;
	QLineEdit *m_entry = nullptr;
	QCheckBox *m_autoscroll = nullptr;

	/* Up and Down walk this, like every other command line an operator has
	 * used. Kept here rather than in a QCompleter because the useful
	 * behaviour is recall in order, not prefix matching. */
	QStringList m_history;
	int m_historyPos = 0;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif /* ARDOP_UI_CONSOLEPAGE_H_ */
