#ifndef ARDOP_UI_FILESPAGE_H_
#define ARDOP_UI_FILESPAGE_H_

#include <QCheckBox>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "aspsession.h"
#include "transcript.h"

extern "C" {
#include "shell/settings.h"
}

/**
 * @file filespage.h
 * @brief Sending a file, receiving one, and knowing where it went.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §3 asks for a send queue, a
 * receive list, per-transfer progress, resume and verify.
 * [17](../../analysis/17-application-protocol.md) §4 supplies one of those
 * answers before the screen is drawn: **there is one transfer per direction**,
 * because concurrency at 300 B/s makes both transfers slower and neither more
 * likely to finish. So there is no queue to draw -- there is a transfer, or there
 * is not.
 *
 * ## The three things an operator has to be able to see
 *
 * **Where files land.** A single fixed directory they chose, never a path
 * derived from the peer (§5). Shown, changeable, and openable, because a
 * transfer that finished into a folder nobody can find has not finished.
 *
 * **Whether to accept.** Auto-accept is off by default and this is where it is
 * turned on. An offer arrives with the name this station would actually create --
 * the sanitised one, not the peer's spelling -- its size, and how much of it is
 * already held from an earlier attempt.
 *
 * **How it is going, and how long it will take.** At a few hundred bytes a
 * second a transfer is measured in minutes to hours, so a bar with no rate on it
 * is not enough information to decide whether to wait.
 *
 * ## Resume is shown, not explained
 *
 * §5's prefix-CRC handshake is the most subtle part of the protocol and none of
 * it belongs on screen. What an operator needs to know is "42% of this is
 * already here", and what they need to be told afterwards is whether it was
 * used -- which is what the transfer log carries.
 */
class FilesPage : public QWidget {
	Q_OBJECT

public:
	explicit FilesPage(AspSession *asp, QWidget *parent = nullptr);

	/** @brief Apply `files.*`, including the receive folder. */
	void applySaved(const ardop_settings *s);

	/** @brief Write `files.*` into @p s. */
	void store(ardop_settings *s) const;

public slots:
	void onStateChanged(int state, const QString &peer);
	void onOfferArrived(const QString &name, quint32 size,
			    quint32 resumable);
	void onProgress(bool inbound, quint32 done, quint32 total);
	void onTransferDone(bool inbound, int result, const QString &path);
	void onNote(const QString &text);

signals:
	/** @brief A `files.*` setting changed and is worth writing to disk. */
	void settingsChanged();

private slots:
	void onChooseFolder();
	void onOpenFolder();
	void onSendFile();
	void onAccept();
	void onReject();
	void onCancelSend();
	void onCancelReceive();

private:
	void updateGate();
	void setFolder(const QString &dir);

	/** @brief What to call the transfer in that direction on screen. */
	QString transferName(bool inbound) const;

	/** @brief The default receive folder, if the settings name none. */
	static QString defaultFolder();

	AspSession *m_asp = nullptr;

	QLabel *m_folder = nullptr;
	QCheckBox *m_autoAccept = nullptr;

	QGroupBox *m_offerBox = nullptr;
	QLabel *m_offerText = nullptr;
	QPushButton *m_accept = nullptr;
	QPushButton *m_reject = nullptr;

	QPushButton *m_sendButton = nullptr;
	QLabel *m_sendLabel = nullptr;
	QProgressBar *m_sendBar = nullptr;
	QPushButton *m_cancelSend = nullptr;

	QLabel *m_recvLabel = nullptr;
	QProgressBar *m_recvBar = nullptr;
	QPushButton *m_cancelRecv = nullptr;

	Transcript *m_log = nullptr;

	QString m_folderPath;
	QString m_sendName;
	QString m_offerName;

	/* Averaged over the whole transfer rather than sampled, because the
	 * instantaneous rate on a half-duplex link is either the full mode rate
	 * or zero depending on whose turn it is, and neither is the number
	 * somebody deciding whether to wait needs. */
	QElapsedTimer m_sendClock;
	QElapsedTimer m_recvClock;

	int m_state = 0;   /**< ::asp_link_state. */
	QString m_lastSentDir;   /**< Where the file chooser opens next. */
};

#endif /* ARDOP_UI_FILESPAGE_H_ */
