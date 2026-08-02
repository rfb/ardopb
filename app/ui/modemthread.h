#ifndef ARDOP_UI_MODEMTHREAD_H_
#define ARDOP_UI_MODEMTHREAD_H_

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

extern "C" {
#include "app/devices.h"
#include "app/spine.h"
#include "app/tnc_host_tcp.h"
}

/**
 * @file modemthread.h
 * @brief The modem, running on a thread of its own.
 *
 * [analysis/14](../../analysis/14-station-application.md) Decision 2:
 *
 * > One runtime, one thread. Nothing outside the modem thread touches
 * > `ardop_runtime` or anything reachable from it, for any reason, ever.
 *
 * This is the first program in which that is a real constraint rather than a
 * property demonstrated by a stress test. `app_step` blocks for around 100 ms
 * inside the capture device, so running it on the interface thread would freeze
 * the window; and the interface thread must never reach past the seam.
 *
 * ## What may be called from where
 *
 * The spine's own header labels every function with its thread, and this class
 * exists so that a Qt programmer does not have to remember which is which:
 *
 * - **This object's public methods are all interface-thread safe.** They either
 *   queue a request on a lock-free ring or read one atomic.
 * - **Nothing here hands out the `app_spine *`.** The one exception is
 *   ::spine, which the display and event pumps need to drain their queues, and
 *   draining is the *consumer* half of the seam -- the half the interface thread
 *   is supposed to own.
 *
 * Note also that the block deliberately adds nothing to `shell/sys.h`.
 * analysis/14 §6 said a portable thread API should be designed when there was a
 * caller to design it against; the caller turned out to be Qt, which brings its
 * own, so the right amount of new platform code is none.
 */
class ModemThread : public QThread {
	Q_OBJECT

public:
	explicit ModemThread(QObject *parent = nullptr);
	~ModemThread() override;

	/** @brief Build the spine and the device manager. @return false on failure. */
	bool open(bool telemetry);

	/** @brief Ask the loop to finish, then wait for it. Safe to call twice. */
	void shutdown();

	/**
	 * @brief The spine, for the display and event pumps only.
	 *
	 * Draining is the consumer half of the seam and belongs to the interface
	 * thread. Anything else through this pointer is a bug -- the submission
	 * calls are wrapped below precisely so that nobody has to reach for it.
	 */
	app_spine *spine() const { return m_spine; }

	/* --- interface thread: asking the modem to do something ---------------- */

	bool requestDevices(const app_device_selection &sel);
	bool requestDeviceClose();
	bool requestPttTest(unsigned ms);

	/**
	 * @brief Ask for the TNC interface on @p port, or 0 to stop it.
	 *
	 * The socket is opened on the *modem thread*, not here, because
	 * `app_set_tnc` writes state the modem thread reads and `spine.h`
	 * divides its API by thread for a reason. Opening a listener is fast --
	 * unlike a device rebuild -- so it costs the loop nothing to do it there,
	 * and it needs no request ring: one atomic carries the whole intent.
	 *
	 * The result arrives as an ::APP_EV_GUEST event, because binding can
	 * fail (the port is the commonest thing on a station to already be in
	 * use) and an operator must be told.
	 */
	void requestHost(int port);

	/** @brief The port currently listening, or 0. */
	int hostPort() const { return m_hostOpen.load(std::memory_order_acquire); }
	bool submitLine(const QString &line);

	/**
	 * @brief Queue a typed link command.
	 *
	 * The typed path rather than a formatted line, for §3's reason: a
	 * command built from an enum cannot be malformed, where one built from a
	 * string can be, and CONNECT is the one command in the program that puts
	 * a transmitter on the air without an operator holding anything down.
	 *
	 * ::ARDOP_CMD_SEND_DATA is rejected by the spine -- it would bypass the
	 * transmit credit. Use the data path.
	 */
	bool submitCmd(const ardop_host_cmd &cmd);

	/**
	 * @brief Connect to @p call at @p bw.
	 *
	 * Separate from submitCmd because the callsign has to be parsed, and a
	 * bad callsign is an operator typo rather than a programming error: it
	 * comes back as a message, not a rejected command.
	 *
	 * @param[out] why  Filled with the reason when this returns false.
	 */
	bool connectTo(const QString &call, ardop_arq_bandwidth bw, QString *why);
	bool submitConfig(app_cfg_key key, const QString &value);
	bool submitConfig(app_cfg_key key, long value);
	bool submitConfig(app_cfg_key key, bool value);

	/** @brief Bytes ::app_tx_submit would take now. */
	size_t txCredit() const;

	/** @brief A snapshot for display. Never for a decision -- see spine.h. */
	void status(app_status *out) const;
	void deviceStatus(app_device_status *out) const;
	app_dev_state deviceState() const;

protected:
	void run() override;

private:
	/** @brief Open or close the listener to match the request. Modem thread. */
	void serviceHost();

	app_spine *m_spine = nullptr;
	app_devices *m_devices = nullptr;
	std::atomic<bool> m_stop { false };

	/* The TNC server. m_hostWanted is written by the interface thread and
	 * read by the modem thread; everything else here belongs to the modem
	 * thread alone. */
	std::atomic<int> m_hostWanted { 0 };
	std::atomic<int> m_hostOpen { 0 };
	ardop_host_tcp *m_host = nullptr;
	app_tnc_ops m_tnc {};
	app_tnc_watch m_watch {};
};

#endif /* ARDOP_UI_MODEMTHREAD_H_ */
