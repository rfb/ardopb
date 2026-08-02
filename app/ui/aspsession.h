#ifndef ARDOP_UI_ASPSESSION_H_
#define ARDOP_UI_ASPSESSION_H_

#include <QObject>
#include <QString>

#include "modemthread.h"

extern "C" {
#include "app/asp_app.h"
}

/**
 * @file aspsession.h
 * @brief The application protocol, bound to the window's thread.
 *
 * [analysis/17](../../analysis/17-application-protocol.md). `app/asp.c` is the
 * protocol and knows nothing about a radio; `app/asp_app.c` binds it to a spine
 * and to real files; this is the third and thinnest layer, and all it does is put
 * the result on the interface thread and turn nine C callbacks into Qt signals.
 *
 * It exists as a class rather than as part of the Chat screen because **two**
 * screens are views of one session: chat and files travel over the same
 * connection, share one transmit credit, and end together when the link drops.
 * Two independent owners of one `asp_app` would be two answers to "are we
 * connected".
 *
 * ## Which thread
 *
 * The interface thread, throughout. `spine.h` divides its API in two: the modem
 * thread produces, and exactly one other thread submits and drains. ASP is
 * entirely on the submitting side -- it calls ::app_tx_submit and it consumes
 * ::APP_EV_RX_DATA -- so it belongs here, beside the widgets, with no lock of its
 * own and no second thread to reason about.
 *
 * ## When a session exists
 *
 * `asp_open` sends HELLO and its documentation says to call it on every new ARQ
 * connection, so that is exactly what happens: a session is opened when the link
 * comes up and closed when it goes down. Nothing is kept across a disconnect
 * except the partial file on disk, which is the whole of §5's resume story.
 *
 * **A session is not opened while a TNC guest is attached, and an open one is
 * closed if a guest arrives.** There is one link, one session and one 16 kB
 * transmit queue ([14](../../analysis/14-station-application.md) Decision 4); if
 * a guest's Pat session and our own file transfer both append to it, the two byte
 * streams interleave and both are destroyed. The spine already refuses our
 * submissions in that state -- this makes the refusal visible rather than leaving
 * a chat window that silently swallows everything typed into it.
 */
class AspSession : public QObject {
	Q_OBJECT

public:
	explicit AspSession(ModemThread *modem, QObject *parent = nullptr);
	~AspSession() override;

	/** @brief Our callsign, for HELLO. Takes effect at the next session. */
	void setCallsign(const QString &call);

	/** @brief Where received files land. Created on demand. */
	void setReceiveDir(const QString &dir);
	QString receiveDir() const { return m_recvDir; }

	/**
	 * @brief Accept offers without asking.
	 *
	 * §5: off by default, because a station that automatically accepts
	 * arbitrary files from any caller is one that will eventually receive
	 * something its operator did not want.
	 */
	void setAutoAccept(bool on);
	bool autoAccept() const { return m_autoAccept; }

	/** @brief ::ASP_LINK_IDLE when no session is open. */
	asp_link_state state() const;

	/** @brief Whether the peer speaks ASP and will take files. */
	bool canSendFiles() const;

	/** @brief The peer's callsign as it introduced itself, or empty. */
	QString peerCall() const;

	/** @brief Whether a transfer is running in that direction. */
	bool sending() const;
	bool receiving() const;

	/**
	 * @brief Whether an ::ardop_link_state carries an ASP session.
	 *
	 * Public and static so it can be asserted without a modem. It is not
	 * "anything but DISC", which is how the Station screen decides whether
	 * its buttons are live, and the two states where it differs are the two
	 * that would be wrong in opposite ways: opening in `ISS_CON_REQ` queues
	 * a HELLO for a connection that may never be answered, and opening in
	 * `FEC_SEND` puts a session protocol into a broadcast with no peer.
	 */
	static bool carriesSession(int state);

public slots:
	/** @brief Follow the link. Opens and closes the session. */
	void onLinkState(int state, const QString &remote);

	/** @brief Follow ownership. A guest attaching ends our session. */
	void onOwnerChanged(bool attached);

	/** @brief One ::APP_EV_RX_DATA. Non-ARQ tags are ignored inside. */
	void onReceived(const QByteArray &tag, const QByteArray &data);

	/**
	 * @brief Move whatever is waiting.
	 *
	 * Driven by the window's pump rather than by a timer here, because the
	 * thing it is waiting for is transmit credit and the pump is already
	 * running at the rate credit changes.
	 */
	void service();

	bool sendText(const QString &text);
	bool sendFile(const QString &path);

	/** @brief Answer the offer last reported by ::offerArrived. */
	bool answerOffer(bool accept);

	/** @brief Cancel the transfer in one direction. The partial is kept. */
	bool cancel(bool inbound);

signals:
	/** @brief The session appeared, changed kind, or ended. */
	void stateChanged(int state, const QString &peer);

	/** @brief A chat line. @p raw when the peer is not speaking ASP. */
	void textArrived(const QString &text, bool raw);

	/**
	 * @brief An offer needs an answer.
	 *
	 * @p name is the *sanitised* name -- what this station would actually
	 * create -- not the one on the wire. A screen that showed the peer's
	 * spelling would be showing something other than what happens.
	 */
	void offerArrived(const QString &name, quint32 size, quint32 resumable);

	/** @brief Bytes moved. */
	void progress(bool inbound, quint32 done, quint32 total);

	/** @brief A transfer ended. @p path is the finished file, or empty. */
	void transferDone(bool inbound, int result, const QString &path);

	/** @brief Something worth showing the operator. */
	void note(const QString &text);

private:
	void openSession();
	void closeSession();
	void reconsider();

	/* The nine hooks, as statics. `ctx` is always `this`. */
	static void onText(void *ctx, const char *text, size_t len, bool raw);
	static void onOffer(void *ctx, const asp_offer *offer,
			    const char *safe_name, uint32_t resumable);
	static void onProgress(void *ctx, bool inbound, uint32_t done,
			       uint32_t total);
	static void onDone(void *ctx, bool inbound, asp_result_code result,
			   const char *path);
	static void onLinkChanged(void *ctx, asp_link_state state,
				  const char *peer_call);
	static void onNote(void *ctx, const char *text);

	ModemThread *m_modem = nullptr;
	asp_app m_app {};
	bool m_open = false;

	bool m_linkUp = false;
	bool m_guestOwned = false;

	QString m_call;
	QString m_recvDir;
	bool m_autoAccept = false;
};

#endif /* ARDOP_UI_ASPSESSION_H_ */
