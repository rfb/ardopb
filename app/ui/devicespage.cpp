#include "devicespage.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

extern "C" {
#include "shell/audio_devices.h"
#include "shell/usbtopo.h"
}

/**
 * @file devicespage.cpp
 * @brief The device screen (see devicespage.h).
 */

namespace {

/* The keying methods, in the order an operator is likely to want them.
 *
 * CAT first because a modern radio with a built-in codec keys that way and
 * ignores RTS entirely -- which is the failure that looks like broken software
 * rather than a wrong setting. */
struct PttMethod {
	const char *label;
	const char *prefix;   /* what goes before the port */
	const char *hint;
};

const PttMethod kMethods[] = {
	{"None (VOX)", "none", "No keying. The radio decides when to transmit."},
	{"CAT: Icom / Xiegu (CI-V)", "civ:",
	 "Every Xiegu emulates Icom CI-V. Append @a4 for a Xiegu X6100/X6200/G90, "
	 "@94 for most Icoms."},
	{"CAT: Kenwood", "kenwood:", "TX; and RX; down the radio's serial port."},
	{"CAT: Yaesu", "yaesu:", "TX1; and TX0; down the radio's serial port."},
	{"Serial RTS", "rts:",
	 "Assert RTS. What a DigiRig Mobile and most simple interfaces use."},
	{"Serial DTR", "dtr:", "Assert DTR instead. Some interfaces wire this one."},
	{"C-Media GPIO (CM108)", "cm108:",
	 "A DigiRig Lite or a cheap USB dongle. Leave the port empty for auto, or "
	 "give /dev/hidrawN. Append +2 for an SSS162x, which has only two pins."},
	{"rigctld", "rigctld:",
	 "Key through a running rigctld. Host:port, or empty for 127.0.0.1:4532."},
};

QString describeState(app_dev_state s)
{
	switch (s) {
	case APP_DEV_CLOSED:  return QObject::tr("closed");
	case APP_DEV_RUNNING: return QObject::tr("running");
	case APP_DEV_FAULTED: return QObject::tr("faulted");
	case APP_DEV_FAILED:  return QObject::tr("did not open");
	}
	return QObject::tr("unknown");
}

}   // namespace

DevicesPage::DevicesPage(ModemThread *modem, QWidget *parent)
	: QWidget(parent), m_modem(modem)
{
	auto *root = new QVBoxLayout(this);

	/* --- detected radios ---------------------------------------------- */
	auto *detBox = new QGroupBox(tr("Detected radios"), this);
	auto *detLayout = new QVBoxLayout(detBox);

	m_detectNote = new QLabel(detBox);
	m_detectNote->setWordWrap(true);
	detLayout->addWidget(m_detectNote);

	m_detected = new QListWidget(detBox);
	m_detected->setMaximumHeight(110);
	detLayout->addWidget(m_detected);
	connect(m_detected, &QListWidget::itemSelectionChanged,
		this, &DevicesPage::onDetectedChosen);

	root->addWidget(detBox);

	/* --- the pickers --------------------------------------------------- */
	auto *pickBox = new QGroupBox(tr("Devices"), this);
	auto *form = new QFormLayout(pickBox);

	m_capture = new QComboBox(pickBox);
	m_playback = new QComboBox(pickBox);
	form->addRow(tr("Capture"), m_capture);
	form->addRow(tr("Playback"), m_playback);

	m_pttMethod = new QComboBox(pickBox);
	for (const auto &m : kMethods)
		m_pttMethod->addItem(QString::fromUtf8(m.label));
	m_pttTarget = new QLineEdit(pickBox);
	m_pttTarget->setPlaceholderText(tr("serial port, or leave empty"));

	form->addRow(tr("Keying"), m_pttMethod);
	form->addRow(tr("Port"), m_pttTarget);

	m_pttHint = new QLabel(pickBox);
	m_pttHint->setWordWrap(true);
	m_pttHint->setStyleSheet("color: palette(mid);");
	form->addRow(QString(), m_pttHint);

	connect(m_pttMethod, &QComboBox::currentIndexChanged, this, [this](int i) {
		if (i >= 0 && i < int(sizeof kMethods / sizeof kMethods[0]))
			m_pttHint->setText(QString::fromUtf8(kMethods[i].hint));
	});
	m_pttMethod->setCurrentIndex(0);
	m_pttHint->setText(QString::fromUtf8(kMethods[0].hint));

	root->addWidget(pickBox);

	/* --- actions ------------------------------------------------------- */
	auto *actions = new QHBoxLayout;
	auto *refreshBtn = new QPushButton(tr("Refresh"), this);
	m_apply = new QPushButton(tr("Apply"), this);
	m_close = new QPushButton(tr("Close devices"), this);
	m_test = new QPushButton(tr("Test PTT"), this);

	m_apply->setDefault(true);
	m_test->setToolTip(tr("Keys the transmitter for one second. Use a dummy "
			      "load."));

	connect(refreshBtn, &QPushButton::clicked, this, &DevicesPage::refresh);
	connect(m_apply, &QPushButton::clicked, this, &DevicesPage::onApply);
	connect(m_close, &QPushButton::clicked, this, &DevicesPage::onClose);
	connect(m_test, &QPushButton::clicked, this, &DevicesPage::onTestPtt);

	actions->addWidget(refreshBtn);
	actions->addStretch(1);
	actions->addWidget(m_close);
	actions->addWidget(m_test);
	actions->addWidget(m_apply);
	root->addLayout(actions);

	m_state = new QLabel(this);
	m_state->setWordWrap(true);
	root->addWidget(m_state);

	root->addStretch(1);

	refresh();
	updateStatus();
}

void DevicesPage::refresh()
{
	fillDevices();
	fillDetected();
}

void DevicesPage::fillDevices()
{
	static ardop_audio_device devs[64];
	const QByteArray backend = m_backend.toUtf8();
	const char *bn = m_backend.isEmpty() ? nullptr : backend.constData();

	for (int dir = 0; dir < 2; dir++) {
		QComboBox *box = dir ? m_playback : m_capture;
		const QString keep = box->currentData().toString();
		box->clear();

		const size_t n = ardop_audio_enumerate(
			dir ? ARDOP_AUDIO_PLAYBACK : ARDOP_AUDIO_CAPTURE, devs,
			sizeof devs / sizeof devs[0], bn);

		box->addItem(tr("System default"), QString());
		for (size_t i = 0; i < n; i++) {
			/* Say the rate here, so an operator learns a device
			 * cannot be used before they pick it and get a refusal
			 * from the open. */
			QString label = QString::fromUtf8(devs[i].name);
			if (devs[i].rate_ok)
				label += tr("  (%1 Hz)").arg(devs[i].native_rate);
			else if (devs[i].native_rate)
				label += tr("  (%1 Hz -- cannot be used)")
						 .arg(devs[i].native_rate);
			if (devs[i].is_default)
				label += tr("  [default]");

			box->addItem(label, QString::fromUtf8(devs[i].id));
			if (!devs[i].rate_ok && devs[i].native_rate) {
				/* Still offered, because refusing to list it
				 * would leave the operator wondering where their
				 * device went. */
				box->setItemData(box->count() - 1,
						 QColor(Qt::darkRed),
						 Qt::ForegroundRole);
			}
		}

		const int at = box->findData(keep);
		if (at >= 0)
			box->setCurrentIndex(at);
	}
}

void DevicesPage::fillDetected()
{
	m_detected->clear();

	if (!ardop_usb_detect_supported()) {
		m_detectNote->setText(
			tr("Detection is not implemented on this platform yet — "
			   "nothing was looked at, which is not the same as "
			   "finding nothing. Choose the devices by hand below."));
		m_detected->setVisible(false);
		return;
	}

	static ardop_radio_candidate cands[16];
	const QByteArray backend = m_backend.toUtf8();
	const size_t n = ardop_radio_detect(
		cands, sizeof cands / sizeof cands[0],
		m_backend.isEmpty() ? nullptr : backend.constData());

	m_detected->setVisible(n > 0);
	if (n == 0) {
		m_detectNote->setText(
			tr("No radios detected. This looks for a keying "
			   "interface on the same USB hardware as a sound card, "
			   "which finds a radio with a built-in codec or an "
			   "interface like a DigiRig. An onboard sound card has "
			   "no USB hardware to search — choose by hand below."));
		return;
	}

	m_detectNote->setText(
		tr("Choosing one fills in the fields below. Nothing is opened "
		   "until you press Apply, and nothing transmits until you press "
		   "Test PTT."));

	for (size_t i = 0; i < n; i++) {
		const ardop_radio_candidate &c = cands[i];
		QString label = c.model[0] ? QString::fromUtf8(c.model)
					   : (c.audio_name[0]
						      ? QString::fromUtf8(c.audio_name)
						      : tr("unknown device"));
		if (c.ptt_spec[0])
			label += tr("  —  %1").arg(QString::fromUtf8(c.ptt_spec));
		else
			label += tr("  —  no keying interface found");

		label += c.link == ARDOP_USB_SAME_DEVICE
				 ? tr("   (same USB device)")
				 : c.link == ARDOP_USB_SAME_HUB
					   ? tr("   (same USB hub)")
					   : QString();

		auto *item = new QListWidgetItem(label, m_detected);
		item->setData(Qt::UserRole, QString::fromUtf8(c.audio_id));
		item->setData(Qt::UserRole + 1, QString::fromUtf8(c.ptt_spec));
	}
}

void DevicesPage::onDetectedChosen()
{
	auto *item = m_detected->currentItem();
	if (!item)
		return;

	const QString audio = item->data(Qt::UserRole).toString();
	const QString ptt = item->data(Qt::UserRole + 1).toString();

	int at = m_capture->findData(audio);
	if (at >= 0)
		m_capture->setCurrentIndex(at);
	at = m_playback->findData(audio);
	if (at >= 0)
		m_playback->setCurrentIndex(at);

	/* Split the suggested spec back into a method and a port, so the fields
	 * show what will be used rather than an opaque string. */
	for (size_t i = 0; i < sizeof kMethods / sizeof kMethods[0]; i++) {
		const QString prefix = QString::fromUtf8(kMethods[i].prefix);
		if (prefix == "none" || !ptt.startsWith(prefix))
			continue;
		m_pttMethod->setCurrentIndex(int(i));
		m_pttTarget->setText(ptt.mid(prefix.length()));
		break;
	}
}

app_device_selection DevicesPage::selection() const
{
	app_device_selection sel {};

	const QByteArray cap = m_capture->currentData().toString().toUtf8();
	const QByteArray play = m_playback->currentData().toString().toUtf8();
	qstrncpy(sel.capture_id, cap.constData(), sizeof sel.capture_id);
	qstrncpy(sel.playback_id, play.constData(), sizeof sel.playback_id);

	/* The names go with the ids, because that pair is what survives a
	 * replug: the id is tried first, then the name. */
	const QByteArray capName =
		m_capture->currentText().section("  (", 0, 0).toUtf8();
	const QByteArray playName =
		m_playback->currentText().section("  (", 0, 0).toUtf8();
	if (!cap.isEmpty())
		qstrncpy(sel.capture_name, capName.constData(),
			 sizeof sel.capture_name);
	if (!play.isEmpty())
		qstrncpy(sel.playback_name, playName.constData(),
			 sizeof sel.playback_name);

	const int mi = m_pttMethod->currentIndex();
	if (mi > 0 && mi < int(sizeof kMethods / sizeof kMethods[0])) {
		const QString spec = QString::fromUtf8(kMethods[mi].prefix) +
				     m_pttTarget->text().trimmed();
		qstrncpy(sel.ptt_spec, spec.toUtf8().constData(),
			 sizeof sel.ptt_spec);
	} else {
		qstrncpy(sel.ptt_spec, "none", sizeof sel.ptt_spec);
	}

	const QByteArray backend = m_backend.toUtf8();
	qstrncpy(sel.backend, backend.constData(), sizeof sel.backend);
	return sel;
}

void DevicesPage::setSelection(const app_device_selection &sel)
{
	int at = m_capture->findData(QString::fromUtf8(sel.capture_id));
	if (at >= 0)
		m_capture->setCurrentIndex(at);
	at = m_playback->findData(QString::fromUtf8(sel.playback_id));
	if (at >= 0)
		m_playback->setCurrentIndex(at);

	const QString spec = QString::fromUtf8(sel.ptt_spec);
	for (size_t i = 0; i < sizeof kMethods / sizeof kMethods[0]; i++) {
		const QString prefix = QString::fromUtf8(kMethods[i].prefix);
		if (prefix == "none")
			continue;
		if (!spec.startsWith(prefix))
			continue;
		m_pttMethod->setCurrentIndex(int(i));
		m_pttTarget->setText(spec.mid(prefix.length()));
		return;
	}
	m_pttMethod->setCurrentIndex(0);
	m_pttTarget->clear();
}

void DevicesPage::onApply()
{
	const app_device_selection sel = selection();
	if (m_modem->requestDevices(sel))
		emit selectionApplied(sel);
}

void DevicesPage::onClose()
{
	m_modem->requestDeviceClose();
}

void DevicesPage::onTestPtt()
{
	m_modem->requestPttTest(1000);
}

void DevicesPage::updateStatus()
{
	app_device_status st {};
	m_modem->deviceStatus(&st);

	QString text = tr("State: %1").arg(describeState(st.state));
	if (st.state == APP_DEV_RUNNING) {
		text += tr("   •   %1 Hz, %2x decimation, %3-sample block")
				.arg(st.device_rate).arg(st.ratio).arg(st.block);
		if (st.ptt_describe[0])
			text += tr("   •   keying %1")
					.arg(QString::fromUtf8(st.ptt_describe));
	}
	if (st.detail[0])
		text += "\n" + QString::fromUtf8(st.detail);

	m_state->setText(text);

	/* Test PTT is only meaningful with a device open, and only safe while
	 * disconnected -- the manager refuses it otherwise, but a button that
	 * does nothing is worse than one that is visibly unavailable. */
	m_test->setEnabled(st.state == APP_DEV_RUNNING);
	m_close->setEnabled(st.state != APP_DEV_CLOSED);
}
