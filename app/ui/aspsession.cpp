#include "aspsession.h"

#include <QDir>

/**
 * @file aspsession.cpp
 * @brief The application protocol on the interface thread (see aspsession.h).
 */

/*
 * Which link states carry an ASP session.
 *
 * Not simply "anything but DISC", which is how the Station screen decides
 * whether its buttons are live, because two of the states that are not DISC are
 * not an ARQ conversation with a peer:
 *
 *   ISS_CON_REQ  a ConReq is out and nothing has answered it. Opening here
 *                would queue a HELLO for a session that may never exist.
 *   FEC_SEND     broadcast, unacknowledged, no peer. §1 gives FEC its own
 *                profile and no file transfer at all; ASP/ARQ has no business
 *                in it.
 */
bool AspSession::carriesSession(int state)
{
	switch (state) {
	case ARDOP_LINK_ISS_CON_ACK:
	case ARDOP_LINK_ISS:
	case ARDOP_LINK_IRS_CON_ACK:
	case ARDOP_LINK_IRS_DATA:
	case ARDOP_LINK_IDLE:
	case ARDOP_LINK_IRS_TO_ISS:
	case ARDOP_LINK_IRS_FROM_ISS:
		return true;
	default:
		return false;
	}
}

AspSession::AspSession(ModemThread *modem, QObject *parent)
	: QObject(parent), m_modem(modem)
{
}

AspSession::~AspSession()
{
	closeSession();
}

void AspSession::setCallsign(const QString &call)
{
	m_call = call.trimmed().toUpper();
}

void AspSession::setReceiveDir(const QString &dir)
{
	m_recvDir = QDir::toNativeSeparators(dir);
}

void AspSession::setAutoAccept(bool on)
{
	m_autoAccept = on;
	/* Takes effect immediately, including on a session already running: an
	 * operator who turns it on because a transfer is waiting means this
	 * one. */
	if (m_open)
		m_app.auto_accept = on;
}

asp_link_state AspSession::state() const
{
	return m_open ? m_app.session.state : ASP_LINK_IDLE;
}

bool AspSession::canSendFiles() const
{
	return m_open && asp_can_send_files(&m_app.session);
}

QString AspSession::peerCall() const
{
	return m_open ? QString::fromUtf8(m_app.session.peer_call) : QString();
}

bool AspSession::sending() const
{
	return m_open && m_app.session.tx_state != ASP_XFER_NONE;
}

bool AspSession::receiving() const
{
	return m_open && m_app.session.rx_state != ASP_XFER_NONE;
}

/* --- lifecycle -------------------------------------------------------------- */

void AspSession::onLinkState(int state, const QString &remote)
{
	(void)remote;
	m_linkUp = carriesSession(state);
	reconsider();
}

void AspSession::onOwnerChanged(bool attached)
{
	m_guestOwned = attached;
	reconsider();
}

void AspSession::reconsider()
{
	const bool want = m_linkUp && !m_guestOwned;
	if (want == m_open)
		return;
	if (want)
		openSession();
	else
		closeSession();
}

void AspSession::openSession()
{
	/* A local: asp_app_open copies the table into the session, so it does
	 * not have to outlive this call -- and a shared static one would be a
	 * second instance's table waiting to happen. */
	asp_app_hooks hooks {};
	hooks.ctx = this;
	hooks.text_arrived = &AspSession::onText;
	hooks.offer_arrived = &AspSession::onOffer;
	hooks.progress = &AspSession::onProgress;
	hooks.transfer_done = &AspSession::onDone;
	hooks.link_changed = &AspSession::onLinkChanged;
	hooks.note = &AspSession::onNote;

	if (!asp_app_open(&m_app, m_modem->spine(), m_call.toUtf8().constData(),
			  m_recvDir.toUtf8().constData(), &hooks)) {
		emit note(tr("cannot use the receive folder %1 -- chat and files "
			     "are unavailable until it is changed")
				  .arg(m_recvDir));
		return;
	}
	m_app.auto_accept = m_autoAccept;
	m_open = true;

	/*
	 * HELLO is already queued by asp_app_open. It does not necessarily go
	 * out now: the answering station is the IRS and cannot transmit until it
	 * gets a turn, which is what AUTOBREAK is for (§7). So the session
	 * begins in HELLO_SENT and the screens say "waiting" rather than
	 * claiming a peer that has not spoken yet.
	 */
	emit stateChanged(m_app.session.state, QString());
}

void AspSession::closeSession()
{
	if (!m_open)
		return;
	m_open = false;
	/* A partial receive stays on disk under .part and is resumable; that is
	 * the whole of the resume story and it is why this is not a failure. */
	asp_app_close(&m_app);
	emit stateChanged(ASP_LINK_IDLE, QString());
}

/* --- driving ---------------------------------------------------------------- */

void AspSession::onReceived(const QByteArray &tag, const QByteArray &data)
{
	if (!m_open)
		return;
	asp_app_rx(&m_app, tag.constData(), data.constData(), size_t(data.size()));
}

void AspSession::service()
{
	if (m_open)
		asp_app_service(&m_app);
}

bool AspSession::sendText(const QString &text)
{
	if (!m_open)
		return false;
	return asp_app_send_text(&m_app, text.toUtf8().constData());
}

bool AspSession::sendFile(const QString &path)
{
	if (!m_open)
		return false;
	return asp_app_send_file(&m_app, path.toUtf8().constData());
}

bool AspSession::answerOffer(bool accept)
{
	if (!m_open)
		return false;
	return asp_app_answer(&m_app, accept);
}

bool AspSession::cancel(bool inbound)
{
	if (!m_open)
		return false;
	return asp_app_cancel(&m_app, inbound);
}

/* --- the hooks -------------------------------------------------------------- */

bool AspSession::looksLikeTyping(const char *p, size_t len)
{
	if (len == 0)
		return true;

	/* A NUL settles it alone: no text contains one, and every binary format
	 * worth worrying about is full of them. */
	if (memchr(p, 0, len))
		return false;

	/*
	 * Otherwise it is a proportion, not a yes/no, and that is the whole
	 * design of this check.
	 *
	 * The obvious test -- "does it decode as UTF-8" -- is wrong in a way
	 * that matters here. §2's raw mode exists precisely for stations that do
	 * not follow this specification, and a plain terminal sending Latin-1
	 * produces a replacement character for every accented letter. Refusing
	 * to show "café" because one byte was not UTF-8 would break the case the
	 * feature is for.
	 *
	 * So: how much of this could not be read? A stray accent is a few per
	 * cent; a PNG or a zip is most of it.
	 */
	const QString s = QString::fromUtf8(p, int(len));
	int bad = 0;
	for (const QChar c : s) {
		const char16_t u = c.unicode();
		if (u == 0xFFFD)
			bad++;
		else if (u < 0x20 && u != '\n' && u != '\r' && u != '\t')
			bad++;
	}
	return bad * 5 < s.size();   /* under 20% unreadable is someone typing */
}

void AspSession::onText(void *ctx, const char *text, size_t len, bool raw)
{
	auto *self = static_cast<AspSession *>(ctx);

	/*
	 * In raw mode every byte received is handed up as chat text, which is
	 * right when the peer is a terminal or ardop-chat and wrong when it is
	 * `ardop-cat` piping a file at us -- measured: a PNG header arrives as
	 * raw chat text. Rendering that fills the transcript with mojibake and
	 * tells the operator nothing about what is actually happening.
	 */
	if (raw && !looksLikeTyping(text, len)) {
		emit self->binaryArrived(qint64(len));
		return;
	}
	emit self->textArrived(QString::fromUtf8(text, int(len)), raw);
}

void AspSession::onOffer(void *ctx, const asp_offer *offer,
			 const char *safe_name, uint32_t resumable)
{
	auto *self = static_cast<AspSession *>(ctx);
	emit self->offerArrived(QString::fromUtf8(safe_name), offer->size,
				resumable);
}

void AspSession::onProgress(void *ctx, bool inbound, uint32_t done,
			    uint32_t total)
{
	auto *self = static_cast<AspSession *>(ctx);
	emit self->progress(inbound, done, total);
}

void AspSession::onDone(void *ctx, bool inbound, asp_result_code result,
			const char *path)
{
	auto *self = static_cast<AspSession *>(ctx);
	emit self->transferDone(inbound, int(result),
				path ? QString::fromUtf8(path) : QString());
}

void AspSession::onLinkChanged(void *ctx, asp_link_state state,
			       const char *peer_call)
{
	auto *self = static_cast<AspSession *>(ctx);
	emit self->stateChanged(int(state), QString::fromUtf8(peer_call));
}

void AspSession::onNote(void *ctx, const char *text)
{
	auto *self = static_cast<AspSession *>(ctx);
	emit self->note(QString::fromUtf8(text));
}
