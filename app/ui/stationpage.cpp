#include "stationpage.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

extern "C" {
#include "codec/frame.h"
#include "codec/stationid.h"
#include "shell/host.h"
}

/**
 * @file stationpage.cpp
 * @brief Station identity and session control (see stationpage.h).
 */

namespace {

/*
 * The choices come from the protocol, not from here.
 *
 * analysis/16 §5's exit criterion is that no protocol name or enum value is
 * written down in the UI. Both lists below are therefore derived: bandwidths by
 * walking the enum through shell/host.h's accessor, and FEC modes by asking
 * core/codec/frame.h to name every frame type there could be and keeping the
 * ones it recognises. Add a data mode to core and it appears here with no change
 * at all -- which is exactly what happened to the last three.
 */
void fill_bandwidths(QComboBox *box)
{
	for (int i = 0;; i++) {
		const char *name =
			ardop_host_bandwidth_name((ardop_arq_bandwidth)i);
		if (!name)
			break;
		/* UNDEFINED is a state the modem can be in, not a choice an
		 * operator can make. */
		if (qstrcmp(name, "UNDEFINED") == 0)
			continue;
		box->addItem(QString::fromUtf8(name), i);
	}
}

void fill_modes(QComboBox *box)
{
	for (int i = 0;; i++) {
		const char *name = ardop_host_mode_name((ardop_link_mode)i);
		if (!name)
			break;
		box->addItem(QString::fromUtf8(name), i);
	}
}

void fill_fec_modes(QComboBox *box)
{
	char name[32];
	for (int t = 0; t < 256; t++) {
		if (!ardop_data_frame_name((uint8_t)t, name, sizeof name))
			continue;
		/* Frame types come in ordered pairs that share a name; keep the
		 * first, since the name is what the protocol takes. */
		if (box->findText(QString::fromUtf8(name)) >= 0)
			continue;
		box->addItem(QString::fromUtf8(name));
	}
}

}   // namespace

StationPage::StationPage(ModemThread *modem, QWidget *parent)
	: QWidget(parent), m_modem(modem)
{
	auto *root = new QVBoxLayout(this);
	root->addWidget(buildIdentity());
	root->addWidget(buildSession());
	root->addStretch(1);

	updateGate();
}

QWidget *StationPage::buildIdentity()
{
	auto *box = new QGroupBox(tr("This station"), this);
	auto *form = new QFormLayout(box);

	m_mycall = new QLineEdit(box);
	m_mycall->setPlaceholderText(tr("your callsign, e.g. N0CALL or N0CALL-4"));
	m_mycall->setMaxLength(APP_CFG_STR_MAX - 1);
	/* Sized to the content. A callsign is at most ten characters, and a
	 * field stretched across the window invites the belief that something
	 * longer belongs in it. */
	m_mycall->setMaximumWidth(220);
	form->addRow(tr("Callsign"), m_mycall);

	/* The one field with a note under it, because it is the one field that
	 * stops the program working when it is wrong or missing. */
	m_mycallNote = new QLabel(box);
	m_mycallNote->setWordWrap(true);
	m_mycallNote->setMinimumWidth(420);
	form->addRow(QString(), m_mycallNote);

	m_grid = new QLineEdit(box);
	m_grid->setPlaceholderText(tr("Maidenhead locator, e.g. FN31pr"));
	m_grid->setMaxLength(APP_CFG_STR_MAX - 1);
	m_grid->setMaximumWidth(220);
	form->addRow(tr("Grid square"), m_grid);

	m_arqbw = new QComboBox(box);
	fill_bandwidths(m_arqbw);
	form->addRow(tr("ARQ bandwidth"), m_arqbw);

	m_protocol = new QComboBox(box);
	fill_modes(m_protocol);
	form->addRow(tr("Protocol mode"), m_protocol);

	m_fecmode = new QComboBox(box);
	fill_fec_modes(m_fecmode);
	form->addRow(tr("FEC mode"), m_fecmode);

	m_fecrepeats = new QSpinBox(box);
	m_fecrepeats->setRange(0, 5);
	form->addRow(tr("FEC repeats"), m_fecrepeats);

	m_busydet = new QSpinBox(box);
	m_busydet->setRange(0, 10);
	m_busydet->setToolTip(tr("0 disables busy detection entirely."));
	form->addRow(tr("Busy sensitivity"), m_busydet);

	auto *flags = new QHBoxLayout;
	m_listen = new QCheckBox(tr("Listen"), box);
	m_listen->setToolTip(tr("Answer connection requests addressed to us."));
	m_autobreak = new QCheckBox(tr("Auto break"), box);
	m_fskonly = new QCheckBox(tr("FSK only"), box);
	m_use600 = new QCheckBox(tr("600 Hz modes"), box);
	m_pingack = new QCheckBox(tr("Answer pings"), box);
	for (QCheckBox *c : {m_listen, m_autobreak, m_fskonly, m_use600, m_pingack})
		flags->addWidget(c);
	flags->addStretch(1);
	form->addRow(QString(), flags);

	connect(m_mycall, &QLineEdit::editingFinished,
		this, &StationPage::onMycallEdited);
	connect(m_grid, &QLineEdit::editingFinished,
		this, &StationPage::onGridEdited);
	connect(m_arqbw, &QComboBox::currentIndexChanged,
		this, &StationPage::onArqBwChanged);
	connect(m_protocol, &QComboBox::currentIndexChanged,
		this, &StationPage::onProtocolModeChanged);
	connect(m_fecmode, &QComboBox::currentIndexChanged,
		this, &StationPage::onFecModeChanged);
	connect(m_fecrepeats, &QSpinBox::valueChanged,
		this, &StationPage::onFecRepeatsChanged);
	connect(m_busydet, &QSpinBox::valueChanged,
		this, &StationPage::onBusyDetChanged);
	for (QCheckBox *c : {m_listen, m_autobreak, m_fskonly, m_use600, m_pingack})
		connect(c, &QCheckBox::toggled, this, &StationPage::onFlagToggled);

	return box;
}

QWidget *StationPage::buildSession()
{
	auto *box = new QGroupBox(tr("Session"), this);
	auto *root = new QVBoxLayout(box);

	auto *row = new QHBoxLayout;
	m_target = new QComboBox(box);
	m_target->setEditable(true);
	m_target->setInsertPolicy(QComboBox::NoInsert);
	m_target->lineEdit()->setPlaceholderText(tr("callsign to connect to"));
	m_target->setMinimumWidth(160);
	row->addWidget(new QLabel(tr("Connect to"), box));
	row->addWidget(m_target, 1);

	m_connect = new QPushButton(tr("Connect"), box);
	m_disconnect = new QPushButton(tr("Disconnect"), box);
	m_abort = new QPushButton(tr("Abort"), box);
	m_break = new QPushButton(tr("Break"), box);
	m_sendid = new QPushButton(tr("Send ID"), box);

	m_connect->setToolTip(tr("Transmits. Calls the station above."));
	m_abort->setToolTip(tr("Stops immediately, without a graceful teardown."));
	m_break->setToolTip(tr("Ask for the link, to become the sending station."));
	m_sendid->setToolTip(tr("Transmits your callsign and grid, once."));

	for (QPushButton *b : {m_connect, m_disconnect, m_abort, m_break, m_sendid})
		row->addWidget(b);
	root->addLayout(row);

	m_sessionState = new QLabel(box);
	root->addWidget(m_sessionState);

	connect(m_connect, &QPushButton::clicked, this, &StationPage::onConnect);
	connect(m_disconnect, &QPushButton::clicked, this, &StationPage::onDisconnect);
	connect(m_abort, &QPushButton::clicked, this, &StationPage::onAbort);
	connect(m_break, &QPushButton::clicked, this, &StationPage::onBreak);
	connect(m_sendid, &QPushButton::clicked, this, &StationPage::onSendId);
	connect(m_target->lineEdit(), &QLineEdit::returnPressed,
		this, &StationPage::onConnect);

	return box;
}

/*
 * The callsign gate.
 *
 * core/link will not open a session without a station ID, so a live Connect
 * button before MYCALL is set produces a failure the operator would reasonably
 * blame on the radio. Better to make the dependency visible: everything that
 * transmits stays disabled, and the note under the field says why.
 */
void StationPage::updateGate()
{
	/* Two independent reasons to be closed, and the operator is told which:
	 * no callsign is something they fix here, a guest holding the link is
	 * something they fix by closing the other program. */
	const bool canTx = m_callsignOk && !m_guestOwned;

	m_connect->setEnabled(canTx && !m_connected);
	m_sendid->setEnabled(canTx);
	m_disconnect->setEnabled(m_connected);
	m_abort->setEnabled(m_connected);
	m_break->setEnabled(m_connected);
	m_target->setEnabled(canTx);

	if (!m_callsignOk) {
		m_mycallNote->setText(
			tr("Set a callsign before transmitting. Nothing on this "
			   "screen can put a signal on the air until this is "
			   "accepted."));
		m_mycallNote->setStyleSheet("color: #c0392b;");
	} else if (m_guestOwned) {
		m_mycallNote->setText(
			tr("A TNC client is attached and owns the link. Its "
			   "session and this one cannot share the transmit "
			   "queue, so these controls resume when it "
			   "disconnects -- see the Guests tab."));
		m_mycallNote->setStyleSheet("color: #d89b2e;");
	} else {
		m_mycallNote->setText(QString());
		m_mycallNote->setStyleSheet(QString());
	}
}

void StationPage::applyExternalChange(const QString &reply)
{
	/* "KEY now VALUE". The canonical reply shape for every setting in the
	 * protocol, which is why it is the thing parsed rather than the command:
	 * the reply is what the modem says it did. */
	const int sep = reply.indexOf(QLatin1String(" now "));
	if (sep <= 0)
		return;
	const QString key = reply.left(sep);
	const QString value = reply.mid(sep + 5).trimmed();
	if (value.isEmpty())
		return;

	/* Suppressed throughout: this is the modem telling us, so echoing it
	 * back as a command would be a loop, and saving it would let a guest
	 * rewrite the operator's file. */
	m_loading = true;

	if (key == QLatin1String("MYCALL")) {
		m_mycall->setText(value);
		/*
		 * Still suppressed, unlike the first version of this, which
		 * cleared the flag before re-running the handler and so echoed
		 * the guest's callsign straight back at the modem *and* saved it
		 * to the operator's file -- the two things the comment three
		 * lines up says this must not do. What the handler is wanted for
		 * is the validation, the gate, and telling the protocol layer
		 * which callsign is now on the air; ::callsignChanged carries
		 * that and is emitted regardless of this flag.
		 */
		onMycallEdited();
		m_loading = false;
		return;
	}
	if (key == QLatin1String("GRIDSQUARE"))
		m_grid->setText(value);
	else if (key == QLatin1String("ARQBW") && m_arqbw->findText(value) >= 0)
		m_arqbw->setCurrentText(value);
	else if (key == QLatin1String("PROTOCOLMODE") &&
		 m_protocol->findText(value) >= 0)
		m_protocol->setCurrentText(value);
	else if (key == QLatin1String("FECMODE") &&
		 m_fecmode->findText(value) >= 0)
		m_fecmode->setCurrentText(value);
	else if (key == QLatin1String("FECREPEATS"))
		m_fecrepeats->setValue(value.toInt());
	else if (key == QLatin1String("BUSYDET"))
		m_busydet->setValue(value.toInt());
	else if (key == QLatin1String("LISTEN"))
		m_listen->setChecked(value == QLatin1String("TRUE"));
	else if (key == QLatin1String("AUTOBREAK"))
		m_autobreak->setChecked(value == QLatin1String("TRUE"));
	else if (key == QLatin1String("FSKONLY"))
		m_fskonly->setChecked(value == QLatin1String("TRUE"));
	else if (key == QLatin1String("USE600MODES"))
		m_use600->setChecked(value == QLatin1String("TRUE"));
	else if (key == QLatin1String("ENABLEPINGACK"))
		m_pingack->setChecked(value == QLatin1String("TRUE"));

	m_loading = false;
}

void StationPage::setGuestOwned(bool owned)
{
	if (owned == m_guestOwned)
		return;
	m_guestOwned = owned;
	updateGate();
}

void StationPage::onMycallEdited()
{
	const QString call = m_mycall->text().trimmed().toUpper();
	if (call != m_mycall->text())
		m_mycall->setText(call);

	/* Validated here as well as by the modem, so the operator learns about a
	 * typo while looking at the field rather than when a connect fails. The
	 * wording is core/'s, so both accounts agree. */
	ardop_stationid id {};
	const ardop_stationid_err err =
		ardop_stationid_from_str(call.toUtf8().constData(), &id);

	m_callsignOk = (err == ARDOP_STATIONID_OK);
	if (!m_callsignOk && !call.isEmpty())
		emit message(tr("callsign %1: %2").arg(
			call, QString::fromUtf8(ardop_stationid_strerror(err))));

	if (m_callsignOk && !m_loading) {
		m_modem->submitConfig(APP_CFG_MYCALL, call);
		emit settingsChanged();
	}
	updateGate();

	/*
	 * Emitted even while loading, and even for a guest's change, because
	 * this one is not about the settings file: it says what callsign this
	 * station is currently operating under, and anything that identifies
	 * with it -- the chat and file protocol's HELLO -- has to agree with the
	 * modem rather than with the file.
	 */
	emit callsignChanged(callsign());
}

void StationPage::onGridEdited()
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_GRIDSQUARE, m_grid->text().trimmed());
	emit settingsChanged();
}

void StationPage::onArqBwChanged(int)
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_ARQBW, m_arqbw->currentText());
	emit settingsChanged();
}

void StationPage::onProtocolModeChanged(int)
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_PROTOCOLMODE, m_protocol->currentText());
	emit settingsChanged();
}

void StationPage::onFecModeChanged(int)
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_FECMODE, m_fecmode->currentText());
	emit settingsChanged();
}

void StationPage::onFecRepeatsChanged(int value)
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_FECREPEATS, (long)value);
	emit settingsChanged();
}

void StationPage::onBusyDetChanged(int value)
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_BUSYDET, (long)value);
	emit settingsChanged();
}

void StationPage::onFlagToggled()
{
	if (m_loading)
		return;
	m_modem->submitConfig(APP_CFG_LISTEN, m_listen->isChecked());
	m_modem->submitConfig(APP_CFG_AUTOBREAK, m_autobreak->isChecked());
	m_modem->submitConfig(APP_CFG_FSKONLY, m_fskonly->isChecked());
	m_modem->submitConfig(APP_CFG_USE600MODES, m_use600->isChecked());
	m_modem->submitConfig(APP_CFG_ENABLEPINGACK, m_pingack->isChecked());
	emit settingsChanged();
}

void StationPage::onConnect()
{
	const QString call = m_target->currentText().trimmed().toUpper();
	if (call.isEmpty()) {
		emit message(tr("no callsign to connect to"));
		return;
	}

	/* The name came out of the box we filled from the same table, so a miss
	 * is not an operator error -- but UNDEFINED is a real setting the link
	 * understands, so falling back to it is honest rather than a guess. */
	ardop_arq_bandwidth bw = ARDOP_ARQ_BW_UNDEFINED;
	if (!ardop_host_bandwidth_for_name(
		    m_arqbw->currentText().toUtf8().constData(), &bw))
		bw = ARDOP_ARQ_BW_UNDEFINED;

	QString why;
	if (!m_modem->connectTo(call, bw, &why)) {
		emit message(tr("connect to %1 refused: %2").arg(call, why));
		return;
	}

	/* Remember it, most recent first, so a repeat call is one click. */
	const int existing = m_target->findText(call);
	if (existing >= 0)
		m_target->removeItem(existing);
	m_target->insertItem(0, call);
	m_target->setCurrentIndex(0);
	while (m_target->count() > 10)
		m_target->removeItem(m_target->count() - 1);

	emit message(tr("connecting to %1 at %2")
			     .arg(call, m_arqbw->currentText()));
	emit settingsChanged();
}

void StationPage::sendSimple(ardop_host_cmd_kind kind, const char *what)
{
	ardop_host_cmd cmd {};
	cmd.kind = kind;
	if (!m_modem->submitCmd(cmd)) {
		emit message(tr("%1 refused: the command queue is full")
				     .arg(QString::fromUtf8(what)));
		return;
	}
	emit message(QString::fromUtf8(what));
}

void StationPage::onDisconnect() { sendSimple(ARDOP_CMD_DISCONNECT, "disconnect"); }
void StationPage::onAbort()      { sendSimple(ARDOP_CMD_ABORT, "abort"); }
void StationPage::onBreak()      { sendSimple(ARDOP_CMD_BREAK, "break"); }
void StationPage::onSendId()     { sendSimple(ARDOP_CMD_SEND_ID, "send ID"); }

void StationPage::setLinkState(const QString &state, const QString &remote)
{
	const bool connected = (state != QLatin1String("DISC"));
	if (connected != m_connected) {
		m_connected = connected;
		updateGate();
	}

	m_sessionState->setText(
		remote.isEmpty()
			? tr("state: %1").arg(state)
			: tr("state: %1, with %2").arg(state, remote));
}

void StationPage::applySaved(const ardop_settings *s)
{
	/*
	 * Filling the fields must not look like an operator editing them: every
	 * handler would fire, each queueing a config command, and the last one
	 * would race the first. So the fields are filled with signals ignored,
	 * and the modem is told once at the end.
	 */
	m_loading = true;

	m_mycall->setText(QString::fromUtf8(
		ardop_settings_get(s, "station.mycall", "")));
	m_grid->setText(QString::fromUtf8(
		ardop_settings_get(s, "station.gridsquare", "")));

	const QString bw = QString::fromUtf8(
		ardop_settings_get(s, "station.arqbw", "2000MAX"));
	if (m_arqbw->findText(bw) >= 0)
		m_arqbw->setCurrentText(bw);

	const QString mode = QString::fromUtf8(
		ardop_settings_get(s, "station.protocolmode", "ARQ"));
	if (m_protocol->findText(mode) >= 0)
		m_protocol->setCurrentText(mode);

	const QString fec = QString::fromUtf8(
		ardop_settings_get(s, "station.fecmode", ""));
	if (!fec.isEmpty() && m_fecmode->findText(fec) >= 0)
		m_fecmode->setCurrentText(fec);

	m_fecrepeats->setValue(
		(int)ardop_settings_get_num(s, "station.fecrepeats", 0));
	m_busydet->setValue(
		(int)ardop_settings_get_num(s, "station.busydet", 5));

	m_listen->setChecked(ardop_settings_get_bool(s, "station.listen", true));
	m_autobreak->setChecked(
		ardop_settings_get_bool(s, "station.autobreak", true));
	m_fskonly->setChecked(ardop_settings_get_bool(s, "station.fskonly", false));
	m_use600->setChecked(
		ardop_settings_get_bool(s, "station.use600modes", false));
	m_pingack->setChecked(
		ardop_settings_get_bool(s, "station.enablepingack", true));

	const QString recent = QString::fromUtf8(
		ardop_settings_get(s, "station.recent", ""));
	for (const QString &call : recent.split(',', Qt::SkipEmptyParts))
		m_target->addItem(call.trimmed());
	m_target->setCurrentText(QString());

	m_loading = false;

	/* Now push the lot, once. The callsign goes through its own handler so
	 * the gate and the validation run; the rest are unconditional. */
	onMycallEdited();
	m_modem->submitConfig(APP_CFG_GRIDSQUARE, m_grid->text());
	m_modem->submitConfig(APP_CFG_ARQBW, m_arqbw->currentText());
	m_modem->submitConfig(APP_CFG_PROTOCOLMODE, m_protocol->currentText());
	if (!m_fecmode->currentText().isEmpty())
		m_modem->submitConfig(APP_CFG_FECMODE, m_fecmode->currentText());
	m_modem->submitConfig(APP_CFG_FECREPEATS, (long)m_fecrepeats->value());
	m_modem->submitConfig(APP_CFG_BUSYDET, (long)m_busydet->value());
	onFlagToggled();
}

QString StationPage::callsign() const
{
	return m_callsignOk ? m_mycall->text().trimmed().toUpper() : QString();
}

void StationPage::store(ardop_settings *s) const
{
	ardop_settings_set(s, "station.mycall",
			   m_mycall->text().toUtf8().constData());
	ardop_settings_set(s, "station.gridsquare",
			   m_grid->text().toUtf8().constData());
	ardop_settings_set(s, "station.arqbw",
			   m_arqbw->currentText().toUtf8().constData());
	ardop_settings_set(s, "station.protocolmode",
			   m_protocol->currentText().toUtf8().constData());
	ardop_settings_set(s, "station.fecmode",
			   m_fecmode->currentText().toUtf8().constData());
	ardop_settings_set_num(s, "station.fecrepeats", m_fecrepeats->value());
	ardop_settings_set_num(s, "station.busydet", m_busydet->value());

	ardop_settings_set_bool(s, "station.listen", m_listen->isChecked());
	ardop_settings_set_bool(s, "station.autobreak", m_autobreak->isChecked());
	ardop_settings_set_bool(s, "station.fskonly", m_fskonly->isChecked());
	ardop_settings_set_bool(s, "station.use600modes", m_use600->isChecked());
	ardop_settings_set_bool(s, "station.enablepingack", m_pingack->isChecked());

	QStringList recent;
	for (int i = 0; i < m_target->count() && i < 10; i++)
		recent << m_target->itemText(i);
	ardop_settings_set(s, "station.recent",
			   recent.join(',').toUtf8().constData());
}
