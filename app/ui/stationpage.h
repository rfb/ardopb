#ifndef ARDOP_UI_STATIONPAGE_H_
#define ARDOP_UI_STATIONPAGE_H_

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

#include "modemthread.h"

extern "C" {
#include "shell/settings.h"
}

/**
 * @file stationpage.h
 * @brief Who this station is, and what it is doing on the air.
 *
 * Two halves with a deliberate line between them, because they answer to
 * different clocks. **Identity and defaults** -- callsign, grid, bandwidth,
 * modes -- are set once and change rarely. **The session** -- connect,
 * disconnect, abort, break -- is what an operator drives during a contact.
 * Mixing them puts a Connect button next to a text field and invites the two
 * most expensive mistakes available here: transmitting with the wrong callsign,
 * and transmitting when you meant to save.
 *
 * ## The callsign is the gate, and it is enforced here
 *
 * Nothing on this screen can start a session until MYCALL has been accepted.
 * That is not decoration: [core/link](../../core/link/link.h) will not open an
 * ARQ session without a station ID, so a Connect button that was live before a
 * callsign was set would simply fail, and the operator would be debugging the
 * radio instead of reading the field they had not filled in.
 *
 * ## Typed commands, not typed text
 *
 * [analysis/14](../../analysis/14-station-application.md) Decision 3: the UI
 * drives the typed API. Everything here goes through ::app_submit_config or
 * ::app_submit_cmd, neither of which can produce a malformed line. The Console
 * screen is where a human may type something malformed on purpose, and it is
 * separate for that reason.
 *
 * ## Applied on edit, saved on apply
 *
 * A setting reaches the modem when the field loses focus or the operator presses
 * Return -- there is no Apply for the identity half, because a settings screen
 * with a global Apply is one where half the fields are lying about the state of
 * the modem. Persistence is the other question, and is answered by the window:
 * this page emits ::settingsChanged and the window writes `station.*` out.
 */
class StationPage : public QWidget {
	Q_OBJECT

public:
	explicit StationPage(ModemThread *modem, QWidget *parent = nullptr);

	/** @brief Load saved values into the fields and push them to the modem. */
	void applySaved(const ardop_settings *s);

	/** @brief Write the current values into @p s under `station.*`. */
	void store(ardop_settings *s) const;

	/**
	 * @brief This station's callsign, or empty if it is not a valid one.
	 *
	 * The chat and file protocol introduces itself with it, and an empty
	 * answer is the right one for a field that has not been filled in yet --
	 * a HELLO carrying half a callsign somebody was in the middle of typing
	 * would identify this station as somebody else.
	 */
	QString callsign() const;

public slots:
	/**
	 * @brief Follow the link state, so the session controls match reality.
	 *
	 * Driven by the window's status pump. What it decides is which buttons
	 * are live: Connect only when disconnected, Disconnect and Abort only
	 * when there is something to end.
	 */
	void setLinkState(const QString &state, const QString &remote);

	/**
	 * @brief A TNC client owns the link, so our own session controls stop.
	 *
	 * The spine already refuses them. This makes the refusal visible, which
	 * matters more than it sounds: a Connect button that silently does
	 * nothing is indistinguishable from a broken radio.
	 */
	void setGuestOwned(bool owned);

	/**
	 * @brief Follow a setting a guest changed.
	 *
	 * [analysis/14](../../analysis/14-station-application.md) Decision 4
	 * accepts a guest's configuration rather than refusing it, and promises
	 * the interface will show what changed. Without this the promise is only
	 * half kept: the Guests screen records that Pat set MYCALL, while this
	 * screen goes on displaying the old one -- and a settings screen showing
	 * a value that is not the modem's is worse than one showing nothing.
	 *
	 * @param reply A canonical `KEY now VALUE` line. Anything else is
	 *              ignored, so this cannot be driven into a bad state by an
	 *              unexpected message.
	 *
	 * **Displayed, not saved.** The running modem takes the guest's value;
	 * the settings file keeps the operator's, so a restart returns to what
	 * the operator chose. A guest borrows this station, it does not
	 * reconfigure it permanently.
	 */
	void applyExternalChange(const QString &reply);

signals:
	/** @brief A setting changed and is worth writing to disk. */
	void settingsChanged();

	/** @brief Something the operator should see in the log. */
	void message(const QString &text);

	/**
	 * @brief The station's callsign changed, from wherever.
	 *
	 * Separate from ::settingsChanged because the two questions have
	 * different answers for a guest's `MYCALL`: the running station has a
	 * new callsign and everything that identifies with it must be told, but
	 * the operator's settings file keeps theirs. Empty when what is in the
	 * field is not a valid callsign.
	 */
	void callsignChanged(const QString &call);

private slots:
	void onMycallEdited();
	void onGridEdited();
	void onArqBwChanged(int index);
	void onProtocolModeChanged(int index);
	void onFecModeChanged(int index);
	void onFecRepeatsChanged(int value);
	void onBusyDetChanged(int value);
	void onFlagToggled();

	void onConnect();
	void onDisconnect();
	void onAbort();
	void onBreak();
	void onSendId();

private:
	QWidget *buildIdentity();
	QWidget *buildSession();
	void sendSimple(ardop_host_cmd_kind kind, const char *what);
	void updateGate();

	ModemThread *m_modem = nullptr;

	QLineEdit *m_mycall = nullptr;
	QLabel *m_mycallNote = nullptr;
	QLineEdit *m_grid = nullptr;
	QComboBox *m_arqbw = nullptr;
	QComboBox *m_protocol = nullptr;
	QComboBox *m_fecmode = nullptr;
	QSpinBox *m_fecrepeats = nullptr;
	QSpinBox *m_busydet = nullptr;

	QCheckBox *m_listen = nullptr;
	QCheckBox *m_autobreak = nullptr;
	QCheckBox *m_fskonly = nullptr;
	QCheckBox *m_use600 = nullptr;
	QCheckBox *m_pingack = nullptr;

	QComboBox *m_target = nullptr;   /**< Editable: recent calls, not a cage. */
	QPushButton *m_connect = nullptr;
	QPushButton *m_disconnect = nullptr;
	QPushButton *m_abort = nullptr;
	QPushButton *m_break = nullptr;
	QPushButton *m_sendid = nullptr;
	QLabel *m_sessionState = nullptr;

	bool m_callsignOk = false;
	bool m_connected = false;
	bool m_guestOwned = false;
	bool m_loading = false;   /**< Suppresses signals while fields are filled. */
};

#endif /* ARDOP_UI_STATIONPAGE_H_ */
