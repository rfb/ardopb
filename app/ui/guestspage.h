#ifndef ARDOP_UI_GUESTSPAGE_H_
#define ARDOP_UI_GUESTSPAGE_H_

#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QWidget>

#include "modemthread.h"

extern "C" {
#include "shell/settings.h"
}

/**
 * @file guestspage.h
 * @brief Letting other programs drive this station, and watching what they do.
 *
 * This is the screen that makes the application useful to anyone already running
 * Pat or Winlink Express: with the TNC interface listening, those talk to *this*
 * program instead of needing a separate `ardopb` beside it, and the operator gets
 * a window on a session they could previously only infer.
 *
 * ## It does not listen until asked
 *
 * The listener binds `INADDR_ANY` -- every interface, not just loopback -- so
 * turning it on makes this station reachable from the rest of the network. That
 * is a decision an operator should make on purpose, so the default is off, and
 * `ardopb` sets the same precedent by requiring an explicit `--host PORT`.
 *
 * Once turned on it is remembered, so it is a first-run decision and not a
 * per-session chore.
 *
 * ## A guest takes over, and that is not a bug
 *
 * There is one link, one session and one 16 kB transmit queue. If a guest's Pat
 * session and this application's own transfer both append to it, the two byte
 * streams interleave and both are destroyed
 * ([14](../../analysis/14-station-application.md) Decision 4). So while a client
 * is attached it *owns* the link and the application's own session controls are
 * refused.
 *
 * Presence is the claim and the TCP disconnect is the release -- there is no
 * handshake to get wedged in. What this screen adds is the explanation: without
 * it, an operator finds a greyed-out Connect button and no reason for it.
 *
 * ## What a guest changes, it changes for real
 *
 * Decision 4 again: Pat sets `MYCALL` during its startup handshake, and refusing
 * it to protect the operator's settings would break the client we are trying to
 * support. So guest configuration is applied -- and every command it ran is
 * listed here, with the answer, because visibility is what was traded for
 * protection.
 */
class GuestsPage : public QWidget {
	Q_OBJECT

public:
	explicit GuestsPage(ModemThread *modem, QWidget *parent = nullptr);

	/** @brief Apply the saved host setting, starting it if it was on. */
	void applySaved(const ardop_settings *s);

	/** @brief Write `host.*` into @p s. */
	void store(ardop_settings *s) const;

public slots:
	/** @brief One ::APP_EV_GUEST event. */
	void onGuestEvent(int code, const QString &text);

	/** @brief Follow the ownership signal. Driven by the window's pump. */
	void setAttached(bool attached);

signals:
	/** @brief The host setting changed and is worth writing to disk. */
	void settingsChanged();

	/** @brief Something the operator should see in the panel log too. */
	void message(const QString &text);

private slots:
	void onToggled(bool on);
	void onPortChanged(int port);

private:
	void updateState();

	ModemThread *m_modem = nullptr;

	QCheckBox *m_enabled = nullptr;
	QSpinBox *m_port = nullptr;
	QLabel *m_state = nullptr;
	QLabel *m_owner = nullptr;
	QPlainTextEdit *m_activity = nullptr;

	bool m_attached = false;
	bool m_loading = false;
};

#endif /* ARDOP_UI_GUESTSPAGE_H_ */
