#include "modemthread.h"

#include <QByteArray>

#include <cstdio>

extern "C" {
#include "codec/stationid.h"
}

/**
 * @file modemthread.cpp
 * @brief The modem loop, on its own thread (see modemthread.h).
 */

ModemThread::ModemThread(QObject *parent) : QThread(parent) {}

ModemThread::~ModemThread()
{
	shutdown();
	app_devices_free(m_devices);
	m_devices = nullptr;
	m_spine = nullptr;   /* freed by shutdown, with the device still alive */
}

bool ModemThread::open(bool telemetry)
{
	app_config cfg {};
	cfg.telemetry = telemetry;

	m_spine = app_open(&cfg);
	if (!m_spine)
		return false;

	m_devices = app_devices_new();
	if (!m_devices) {
		app_close(m_spine);
		m_spine = nullptr;
		return false;
	}
	return true;
}

void ModemThread::run()
{
	/*
	 * The whole of the modem thread. app_devices_service applies whatever the
	 * interface asked for and polls both fault latches; app_step drives one
	 * iteration of the modem.
	 *
	 * Neither call is fast. app_step blocks for about a device period inside
	 * capture -- which is what paces this loop and why it is not on the
	 * interface thread -- and a device rebuild inside app_devices_service can
	 * take hundreds of milliseconds on a cold endpoint. With nothing bound,
	 * app_devices_service sleeps instead, so an idle program does not spin.
	 */
	while (!m_stop.load(std::memory_order_acquire)) {
		serviceHost();
		app_devices_service(m_devices, m_spine);
		app_step(m_spine);
	}

	/* Stop listening before the spine goes. app_step is what services the
	 * transport, so a socket left open past this point is one nothing is
	 * reading -- a client would connect and hang. */
	requestHost(0);
	serviceHost();

	/*
	 * Teardown order, and it is the one order that cannot leave a transmitter
	 * keyed: detach the spine, close the audio backend, then the keying line.
	 * app_devices_close does the first two and app_close unkeys through the
	 * platform that is still bound, so app_close runs first.
	 */
	app_close(m_spine);
	app_devices_close(m_devices, nullptr);
}

void ModemThread::requestHost(int port)
{
	m_hostWanted.store(port, std::memory_order_release);
}

void ModemThread::serviceHost()
{
	const int want = m_hostWanted.load(std::memory_order_acquire);
	const int have = m_hostOpen.load(std::memory_order_relaxed);
	if (want == have)
		return;

	/* Always tear down first, even when moving between two ports: two
	 * listeners would mean two hosts, and the whole model is that there is
	 * one. */
	if (m_host) {
		app_set_tnc(m_spine, nullptr);
		ardop_host_tcp_close(m_host);
		m_host = nullptr;
		m_hostOpen.store(0, std::memory_order_release);
		app_report_guest(m_spine, APP_GUEST_STOPPED,
				 "TNC interface stopped");
	}

	if (want <= 0)
		return;

	m_host = ardop_host_tcp_open((uint16_t)want);
	if (!m_host) {
		char msg[APP_TEXT_MAX];
		snprintf(msg, sizeof msg,
			 "could not listen on %d and %d -- something else is "
			 "using them",
			 want, want + 1);
		app_report_guest(m_spine, APP_GUEST_LISTEN_FAILED, msg);
		/* Cleared so the interface's request and reality agree; an
		 * operator who fixes the conflict asks again. */
		m_hostWanted.store(0, std::memory_order_release);
		return;
	}

	app_tnc_host_tcp_bind(&m_tnc, m_host);
	app_tnc_host_tcp_watch(&m_watch, m_host, m_spine);
	app_set_tnc(m_spine, &m_tnc);
	m_hostOpen.store(want, std::memory_order_release);
}

void ModemThread::shutdown()
{
	if (!isRunning())
		return;
	m_stop.store(true, std::memory_order_release);
	/* Generous: one iteration is a device period, and a rebuild in flight is
	 * longer. Terminating a thread that owns a keyed transmitter is not an
	 * option, so this waits rather than gives up. */
	wait(10000);
}

/* --- interface thread ------------------------------------------------------ */

bool ModemThread::requestDevices(const app_device_selection &sel)
{
	return m_devices && app_devices_request(m_devices, &sel);
}

bool ModemThread::requestDeviceClose()
{
	return m_devices && app_devices_request_close(m_devices);
}

bool ModemThread::requestPttTest(unsigned ms)
{
	return m_devices && app_devices_request_ptt_test(m_devices, ms);
}

bool ModemThread::submitLine(const QString &line)
{
	if (!m_spine)
		return false;
	const QByteArray utf8 = line.toUtf8();
	return app_submit_line(m_spine, utf8.constData());
}

bool ModemThread::submitConfig(app_cfg_key key, const QString &value)
{
	if (!m_spine)
		return false;
	const QByteArray utf8 = value.toUtf8();
	app_cfg_value v {};
	v.str = utf8.constData();
	/* The string is copied into the queue slot, so the QByteArray only has to
	 * outlive this call -- which it does. */
	return app_submit_config(m_spine, key, v);
}

bool ModemThread::submitCmd(const ardop_host_cmd &cmd)
{
	if (!m_spine)
		return false;
	return app_submit_cmd(m_spine, &cmd);
}

bool ModemThread::connectTo(const QString &call, ardop_arq_bandwidth bw,
			    QString *why)
{
	if (!m_spine) {
		if (why)
			*why = tr("the modem is not running");
		return false;
	}

	const QByteArray utf8 = call.trimmed().toUpper().toUtf8();
	ardop_stationid target {};
	const ardop_stationid_err err =
		ardop_stationid_from_str(utf8.constData(), &target);
	if (err != ARDOP_STATIONID_OK) {
		/* core/ owns the wording, so the operator and the TNC protocol
		 * give the same account of a bad callsign. */
		if (why)
			*why = QString::fromUtf8(ardop_stationid_strerror(err));
		return false;
	}

	ardop_host_cmd cmd {};
	cmd.kind = ARDOP_CMD_CONNECT;
	cmd.target = target;
	cmd.bandwidth = bw;
	if (!app_submit_cmd(m_spine, &cmd)) {
		if (why)
			*why = tr("the command queue is full");
		return false;
	}
	return true;
}

bool ModemThread::submitConfig(app_cfg_key key, long value)
{
	if (!m_spine)
		return false;
	app_cfg_value v {};
	v.num = value;
	return app_submit_config(m_spine, key, v);
}

bool ModemThread::submitConfig(app_cfg_key key, bool value)
{
	if (!m_spine)
		return false;
	app_cfg_value v {};
	v.flag = value;
	return app_submit_config(m_spine, key, v);
}

size_t ModemThread::txCredit() const
{
	return m_spine ? app_tx_credit(m_spine) : 0;
}

void ModemThread::status(app_status *out) const
{
	if (m_spine)
		app_snapshot(m_spine, out);
	else
		*out = app_status {};
}

void ModemThread::deviceStatus(app_device_status *out) const
{
	if (m_devices)
		app_devices_status(m_devices, out);
	else
		*out = app_device_status {};
}

app_dev_state ModemThread::deviceState() const
{
	return m_devices ? app_devices_state(m_devices) : APP_DEV_CLOSED;
}
