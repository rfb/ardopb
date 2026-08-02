#include "guestspage.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

/**
 * @file guestspage.cpp
 * @brief The TNC interface and its guests (see guestspage.h).
 */

namespace {
/* Where an ARDOP TNC is expected to be. Pat and Winlink both default to it, and
 * the data channel is always the next port up. */
constexpr int kDefaultPort = 8515;
}   // namespace

GuestsPage::GuestsPage(ModemThread *modem, QWidget *parent)
	: QWidget(parent), m_modem(modem)
{
	auto *root = new QVBoxLayout(this);

	auto *box = new QGroupBox(tr("TNC interface"), this);
	auto *form = new QFormLayout(box);

	auto *row = new QHBoxLayout;
	m_enabled = new QCheckBox(tr("Accept connections from other programs"),
				  box);
	m_enabled->setToolTip(
		tr("Lets Pat, Winlink Express or any ARDOP client drive this "
		   "station. Off by default: the port is reachable from your "
		   "whole network, not just this machine."));
	row->addWidget(m_enabled);
	row->addStretch(1);
	form->addRow(row);

	m_port = new QSpinBox(box);
	m_port->setRange(1024, 65534);   /* +1 must also be a valid port */
	m_port->setValue(kDefaultPort);
	m_port->setToolTip(tr("The command channel. The data channel is always "
			      "this plus one."));
	m_port->setMaximumWidth(120);
	form->addRow(tr("Port"), m_port);

	m_state = new QLabel(box);
	m_state->setWordWrap(true);
	form->addRow(QString(), m_state);

	root->addWidget(box);

	m_owner = new QLabel(this);
	m_owner->setWordWrap(true);
	root->addWidget(m_owner);

	root->addWidget(new QLabel(tr("Activity"), this));
	m_activity = new QPlainTextEdit(this);
	m_activity->setReadOnly(true);
	m_activity->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	m_activity->setMaximumBlockCount(2000);
	root->addWidget(m_activity, 1);

	connect(m_enabled, &QCheckBox::toggled, this, &GuestsPage::onToggled);
	connect(m_port, &QSpinBox::valueChanged, this,
		&GuestsPage::onPortChanged);

	updateState();
}

void GuestsPage::onToggled(bool on)
{
	m_port->setEnabled(!on);   /* changing it while listening is a restart */
	if (!m_loading) {
		m_modem->requestHost(on ? m_port->value() : 0);
		emit settingsChanged();
	}
	updateState();
}

void GuestsPage::onPortChanged(int port)
{
	if (m_loading)
		return;
	/* Only meaningful while stopped -- the box is disabled otherwise -- so
	 * this just records the choice for the next start. */
	(void)port;
	emit settingsChanged();
}

void GuestsPage::setAttached(bool attached)
{
	if (attached == m_attached)
		return;
	m_attached = attached;
	updateState();
}

void GuestsPage::updateState()
{
	const int listening = m_modem->hostPort();

	if (listening > 0)
		m_state->setText(tr("Listening on %1 (commands) and %2 (data). "
				    "Point your client at this machine on "
				    "port %1.")
					 .arg(listening)
					 .arg(listening + 1));
	else if (m_enabled->isChecked())
		m_state->setText(tr("Starting..."));
	else
		m_state->setText(tr("Not listening. Nothing can connect."));

	/*
	 * The explanation for a greyed-out Connect button on another screen.
	 * Without it that button is simply broken as far as an operator can
	 * tell.
	 */
	if (m_attached) {
		m_owner->setText(
			tr("A client is attached and owns the link. This "
			   "program's own Connect, Disconnect and Send ID are "
			   "refused until it disconnects -- there is one "
			   "session and one transmit queue, and two writers "
			   "would destroy both streams."));
		m_owner->setStyleSheet("color: #d89b2e;");
	} else {
		m_owner->setText(tr("No client attached. This program owns the "
				    "link."));
		m_owner->setStyleSheet(QString());
	}
}

void GuestsPage::onGuestEvent(int code, const QString &text)
{
	const char *colour = "#7f8c8d";
	switch (code) {
	case APP_GUEST_CONNECTED:
	case APP_GUEST_LISTENING:
		colour = "#2ecc71";
		break;
	case APP_GUEST_DISCONNECTED:
	case APP_GUEST_STOPPED:
		colour = "#7f8c8d";
		break;
	case APP_GUEST_REFUSED:
	case APP_GUEST_LISTEN_FAILED:
		colour = "#c0392b";
		break;
	case APP_GUEST_COMMAND:
		colour = "#4fa3e8";
		break;
	}

	m_activity->appendHtml(
		QStringLiteral("<span style='color:#7f8c8d'>%1</span> "
			       "<span style='color:%2'>%3</span>")
			.arg(QDateTime::currentDateTime().toString("HH:mm:ss"),
			     QString::fromUtf8(colour), text.toHtmlEscaped()));

	if (code == APP_GUEST_LISTEN_FAILED) {
		/* The request failed on the modem thread, which already cleared
		 * it; the checkbox has to follow or it claims something untrue. */
		m_loading = true;
		m_enabled->setChecked(false);
		m_port->setEnabled(true);
		m_loading = false;
		emit message(text);
		emit settingsChanged();
	}

	/* Anything that changes whether we are listening. */
	if (code == APP_GUEST_LISTENING || code == APP_GUEST_STOPPED ||
	    code == APP_GUEST_LISTEN_FAILED)
		updateState();
}

void GuestsPage::applySaved(const ardop_settings *s)
{
	m_loading = true;
	m_port->setValue(
		(int)ardop_settings_get_num(s, "host.port", kDefaultPort));
	const bool on = ardop_settings_get_bool(s, "host.enabled", false);
	m_enabled->setChecked(on);
	m_port->setEnabled(!on);
	m_loading = false;

	/* Started here rather than in the toggle handler, because loading
	 * deliberately suppressed that -- but a station that was hosting when it
	 * was last closed should be hosting when it opens. */
	if (on)
		m_modem->requestHost(m_port->value());
	updateState();
}

void GuestsPage::store(ardop_settings *s) const
{
	ardop_settings_set_bool(s, "host.enabled", m_enabled->isChecked());
	ardop_settings_set_num(s, "host.port", m_port->value());
}
