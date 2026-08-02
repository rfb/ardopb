#include "filespage.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLocale>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

/**
 * @file filespage.cpp
 * @brief The files screen (see filespage.h).
 */

namespace {

QString human_bytes(quint64 n)
{
	return QLocale::system().formattedDataSize(qint64(n),
						   n < 1024 ? 0 : 1,
						   QLocale::DataSizeTraditionalFormat);
}

/* "about 3 min", "about 1 h 20 min". Rounded on purpose: a countdown accurate to
 * the second on a link whose throughput changes with the mode and the channel is
 * a precision nobody should be invited to trust. */
QString human_eta(double seconds)
{
	if (seconds < 45)
		return QObject::tr("under a minute");
	const int mins = int(seconds / 60.0 + 0.5);
	if (mins < 60)
		return QObject::tr("about %n min", nullptr, mins);
	const int hours = mins / 60;
	const int rest = mins % 60;
	return rest ? QObject::tr("about %1 h %2 min").arg(hours).arg(rest)
		    : QObject::tr("about %n h", nullptr, hours);
}

QString progress_text(quint32 done, quint32 total, const QElapsedTimer &clock)
{
	QString s = QObject::tr("%1 of %2")
			    .arg(human_bytes(done), human_bytes(total));
	const qint64 ms = clock.isValid() ? clock.elapsed() : 0;
	if (ms < 3000 || done == 0)
		return s;

	const double rate = double(done) * 1000.0 / double(ms);
	s += QObject::tr(", %1 B/s").arg(rate, 0, 'f', rate < 100 ? 1 : 0);
	if (done < total)
		s += QObject::tr(", %1 left")
			     .arg(human_eta(double(total - done) / rate));
	return s;
}

const char *result_text(int result)
{
	switch (result) {
	case ASP_RESULT_OK:
		return QT_TR_NOOP("complete, and the checksum matches");
	case ASP_RESULT_CRC_MISMATCH:
		return QT_TR_NOOP("the checksum did not match; the partial is "
				  "kept and can be resumed");
	case ASP_RESULT_SHORT:
		return QT_TR_NOOP("ended early; the partial is kept and can be "
				  "resumed");
	case ASP_RESULT_WRITE_FAILED:
		return QT_TR_NOOP("could not be written to the receive folder");
	default:
		return QT_TR_NOOP("ended");
	}
}

}   // namespace

FilesPage::FilesPage(AspSession *asp, QWidget *parent)
	: QWidget(parent), m_asp(asp)
{
	auto *root = new QVBoxLayout(this);

	/* --- where things land ------------------------------------------- */
	auto *folderBox = new QGroupBox(tr("Receive folder"), this);
	auto *folderLayout = new QVBoxLayout(folderBox);

	auto *folderRow = new QHBoxLayout;
	m_folder = new QLabel(this);
	m_folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_folder->setWordWrap(true);
	folderRow->addWidget(m_folder, 1);

	auto *change = new QPushButton(tr("Change…"), this);
	auto *open = new QPushButton(tr("Open"), this);
	folderRow->addWidget(change);
	folderRow->addWidget(open);
	folderLayout->addLayout(folderRow);

	m_autoAccept = new QCheckBox(
		tr("Accept offered files without asking"), this);
	m_autoAccept->setToolTip(
		tr("Off by default. A station that automatically accepts "
		   "arbitrary files from any caller will eventually receive "
		   "something its operator did not want."));
	folderLayout->addWidget(m_autoAccept);
	root->addWidget(folderBox);

	/* --- an offer waiting for an answer ------------------------------- */
	m_offerBox = new QGroupBox(tr("Offered to this station"), this);
	auto *offerLayout = new QHBoxLayout(m_offerBox);
	m_offerText = new QLabel(this);
	m_offerText->setWordWrap(true);
	offerLayout->addWidget(m_offerText, 1);
	m_accept = new QPushButton(tr("Accept"), this);
	m_reject = new QPushButton(tr("Reject"), this);
	offerLayout->addWidget(m_accept);
	offerLayout->addWidget(m_reject);
	m_offerBox->setVisible(false);
	root->addWidget(m_offerBox);

	/* --- the two transfers -------------------------------------------- */
	auto *sendBox = new QGroupBox(tr("Sending"), this);
	auto *sendLayout = new QVBoxLayout(sendBox);
	auto *sendRow = new QHBoxLayout;
	m_sendButton = new QPushButton(tr("Send a file…"), this);
	sendRow->addWidget(m_sendButton);
	m_sendLabel = new QLabel(tr("nothing being sent"), this);
	m_sendLabel->setWordWrap(true);
	sendRow->addWidget(m_sendLabel, 1);
	m_cancelSend = new QPushButton(tr("Cancel"), this);
	m_cancelSend->setEnabled(false);
	sendRow->addWidget(m_cancelSend);
	sendLayout->addLayout(sendRow);
	m_sendBar = new QProgressBar(this);
	m_sendBar->setRange(0, 100);
	m_sendBar->setVisible(false);
	sendLayout->addWidget(m_sendBar);
	root->addWidget(sendBox);

	auto *recvBox = new QGroupBox(tr("Receiving"), this);
	auto *recvLayout = new QVBoxLayout(recvBox);
	auto *recvRow = new QHBoxLayout;
	m_recvLabel = new QLabel(tr("nothing being received"), this);
	m_recvLabel->setWordWrap(true);
	recvRow->addWidget(m_recvLabel, 1);
	m_cancelRecv = new QPushButton(tr("Cancel"), this);
	m_cancelRecv->setEnabled(false);
	recvRow->addWidget(m_cancelRecv);
	recvLayout->addLayout(recvRow);
	m_recvBar = new QProgressBar(this);
	m_recvBar->setRange(0, 100);
	m_recvBar->setVisible(false);
	recvLayout->addWidget(m_recvBar);
	root->addWidget(recvBox);

	m_log = new Transcript(false, this);
	/* The four groups above have fixed heights, so a stretch factor alone
	 * leaves the log a two-line slot on a window this size. It is the record
	 * of what was sent and received, which is the part somebody comes back
	 * to, so it gets a floor of its own. */
	m_log->setMinimumHeight(140);
	root->addWidget(m_log, 1);

	connect(change, &QPushButton::clicked, this, &FilesPage::onChooseFolder);
	connect(open, &QPushButton::clicked, this, &FilesPage::onOpenFolder);
	connect(m_sendButton, &QPushButton::clicked, this, &FilesPage::onSendFile);
	connect(m_accept, &QPushButton::clicked, this, &FilesPage::onAccept);
	connect(m_reject, &QPushButton::clicked, this, &FilesPage::onReject);
	connect(m_cancelSend, &QPushButton::clicked, this,
		&FilesPage::onCancelSend);
	connect(m_cancelRecv, &QPushButton::clicked, this,
		&FilesPage::onCancelReceive);
	connect(m_autoAccept, &QCheckBox::toggled, this, [this](bool on) {
		m_asp->setAutoAccept(on);
		emit settingsChanged();
	});

	setFolder(defaultFolder());
	updateGate();
}

/*
 * Downloads, and not the configuration directory.
 *
 * `ardop_config_dir` is where settings belong and it is a dotted directory on
 * Linux -- a perfectly good place for a file nobody is meant to open by hand,
 * and a bad one for a file somebody just waited twenty minutes to receive.
 * QStandardPaths knows where each platform actually puts arriving files, and
 * this is the one place in the tree where the Qt answer is better than the
 * portable C one.
 */
QString FilesPage::defaultFolder()
{
	QString base =
		QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	if (base.isEmpty())
		base = QStandardPaths::writableLocation(
			QStandardPaths::DocumentsLocation);
	if (base.isEmpty())
		base = QDir::homePath();
	return QDir(base).filePath(QStringLiteral("ardop"));
}

void FilesPage::setFolder(const QString &dir)
{
	m_folderPath = QDir::toNativeSeparators(dir);
	m_folder->setText(m_folderPath);
	m_asp->setReceiveDir(m_folderPath);
}

void FilesPage::applySaved(const ardop_settings *s)
{
	const QString dir =
		QString::fromUtf8(ardop_settings_get(s, "files.recv_dir", ""));
	setFolder(dir.isEmpty() ? defaultFolder() : dir);

	const bool believe =
		ardop_settings_get_bool(s, "files.auto_accept", false);
	m_autoAccept->setChecked(believe);
	m_asp->setAutoAccept(believe);

	m_lastSentDir =
		QString::fromUtf8(ardop_settings_get(s, "files.send_dir", ""));
}

void FilesPage::store(ardop_settings *s) const
{
	ardop_settings_set(s, "files.recv_dir", m_folderPath.toUtf8().constData());
	ardop_settings_set_bool(s, "files.auto_accept",
				m_autoAccept->isChecked());
	ardop_settings_set(s, "files.send_dir", m_lastSentDir.toUtf8().constData());
}

void FilesPage::onChooseFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(
		this, tr("Where should received files go?"), m_folderPath);
	if (dir.isEmpty())
		return;
	setFolder(dir);
	emit settingsChanged();
	m_log->append(QStringLiteral("--"),
		      tr("received files will go to %1").arg(m_folderPath));
}

void FilesPage::onOpenFolder()
{
	/* Created on the way, because the commonest reason this fails is that
	 * nothing has been received yet and the folder does not exist. */
	QDir().mkpath(m_folderPath);
	QDesktopServices::openUrl(QUrl::fromLocalFile(m_folderPath));
}

void FilesPage::onSendFile()
{
	const QString path = QFileDialog::getOpenFileName(
		this, tr("Send which file?"),
		m_lastSentDir.isEmpty() ? QDir::homePath() : m_lastSentDir);
	if (path.isEmpty())
		return;

	m_lastSentDir = QFileInfo(path).absolutePath();
	emit settingsChanged();

	/*
	 * The whole file is read once, here, to compute the CRC that OFFER
	 * carries -- §5, and there is no way to fill the message in without it.
	 * On a slow disk that is a visible pause in the interface. It is
	 * accepted rather than moved to a thread: beside what follows on an HF
	 * link it is nothing, and a background CRC would need its own
	 * cancellation and its own way of saying the file changed underneath it.
	 */
	if (!m_asp->sendFile(path)) {
		m_log->append(QStringLiteral("!!"),
			      tr("could not offer %1").arg(path), "#c0392b");
		return;
	}

	m_sendName = QFileInfo(path).fileName();
	m_sendClock.start();
	m_log->append(QStringLiteral(">>"),
		      tr("offered %1; waiting for the other station to answer")
			      .arg(m_sendName),
		      "#4fa3e8");
	updateGate();
}

void FilesPage::onAccept()
{
	m_asp->answerOffer(true);
	m_offerBox->setVisible(false);
	m_recvClock.start();
	m_log->append(QStringLiteral("<<"),
		      tr("accepted %1").arg(m_offerName), "#4fa3e8");
}

void FilesPage::onReject()
{
	m_asp->answerOffer(false);
	m_offerBox->setVisible(false);
	m_log->append(QStringLiteral("<<"),
		      tr("refused %1").arg(m_offerName), "#d89b2e");
}

void FilesPage::onCancelSend()
{
	m_asp->cancel(false);
	m_log->append(QStringLiteral("--"),
		      tr("cancelled sending %1; the other station keeps what "
			 "arrived and can resume it")
			      .arg(m_sendName),
		      "#d89b2e");
}

void FilesPage::onCancelReceive()
{
	m_asp->cancel(true);
	m_log->append(QStringLiteral("--"),
		      tr("cancelled receiving %1; what arrived is kept and can "
			 "be resumed")
			      .arg(m_offerName),
		      "#d89b2e");
}

void FilesPage::onStateChanged(int state, const QString &peer)
{
	(void)peer;
	m_state = state;
	if (state == ASP_LINK_IDLE) {
		m_offerBox->setVisible(false);
		m_sendBar->setVisible(false);
		m_recvBar->setVisible(false);
		m_sendLabel->setText(tr("nothing being sent"));
		m_recvLabel->setText(tr("nothing being received"));
	}
	updateGate();
}

void FilesPage::updateGate()
{
	const bool files = m_asp->canSendFiles();
	m_sendButton->setEnabled(files && !m_asp->sending());
	m_cancelSend->setEnabled(m_asp->sending());
	m_cancelRecv->setEnabled(m_asp->receiving());

	if (!files) {
		/* Two different reasons, and an operator can act on the
		 * difference: one waits, the other does not. */
		m_sendButton->setToolTip(
			m_state == ASP_LINK_RAW
				? tr("The other station is not running this "
				     "program. File transfer needs both ends "
				     "to speak the protocol; chat still works.")
				: tr("No session. Connect to a station from the "
				     "Station screen, or wait for one to call."));
	} else {
		m_sendButton->setToolTip(QString());
	}
}

void FilesPage::onOfferArrived(const QString &name, quint32 size,
			       quint32 resumable)
{
	m_offerName = name;
	m_recvClock.start();

	QString text = tr("%1 is offering %2 (%3).")
			       .arg(m_asp->peerCall().isEmpty()
					    ? tr("The other station")
					    : m_asp->peerCall(),
				    name, human_bytes(size));
	if (resumable)
		text += tr(" %1 of it is already here from an earlier attempt "
			   "and will be resumed if it matches.")
				.arg(human_bytes(resumable));
	text += tr(" It will be written to %1.").arg(m_folderPath);

	m_offerText->setText(text);
	m_log->append(QStringLiteral("<<"), text, "#d89b2e");

	/*
	 * With auto-accept on the session has already answered, so there is
	 * nothing to ask -- but the offer is still logged above, because a file
	 * appearing in a folder with no record of where it came from is exactly
	 * what an unattended station should not do.
	 */
	m_offerBox->setVisible(!m_autoAccept->isChecked());
	updateGate();
}

void FilesPage::onProgress(bool inbound, quint32 done, quint32 total)
{
	QProgressBar *bar = inbound ? m_recvBar : m_sendBar;
	QLabel *label = inbound ? m_recvLabel : m_sendLabel;
	const QElapsedTimer &clock = inbound ? m_recvClock : m_sendClock;
	const QString name = transferName(inbound);

	bar->setVisible(true);
	bar->setValue(total ? int(qint64(done) * 100 / total) : 0);
	label->setText(QStringLiteral("%1 — %2").arg(
		name, progress_text(done, total, clock)));
	updateGate();
}

/*
 * A transfer is normally named by the offer that started it -- but a session can
 * be handed one that this screen never saw begin: a resume picked up by a peer
 * that reconnects, or an offer auto-accepted before the tab was ever opened. A
 * progress bar labelled with an empty string reads as a bug; this reads as what
 * it is.
 */
QString FilesPage::transferName(bool inbound) const
{
	const QString name = inbound ? m_offerName : m_sendName;
	return name.isEmpty() ? tr("a file") : name;
}

void FilesPage::onTransferDone(bool inbound, int result, const QString &path)
{
	const QString name = transferName(inbound);
	const bool ok = result == ASP_RESULT_OK;

	QString text = tr("%1: %2").arg(name, tr(result_text(result)));
	if (inbound && ok && !path.isEmpty())
		text = tr("%1 received — %2")
			       .arg(name, QDir::toNativeSeparators(path));

	m_log->append(inbound ? QStringLiteral("<<") : QStringLiteral(">>"), text,
		      ok ? "#2ecc71" : "#c0392b");

	QProgressBar *bar = inbound ? m_recvBar : m_sendBar;
	QLabel *label = inbound ? m_recvLabel : m_sendLabel;
	bar->setVisible(false);
	label->setText(inbound ? tr("nothing being received")
			       : tr("nothing being sent"));
	if (inbound)
		m_offerBox->setVisible(false);
	updateGate();
}

void FilesPage::onNote(const QString &text)
{
	m_log->append(QStringLiteral("--"), text, "#7f8c8d");
}
