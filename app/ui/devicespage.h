#ifndef ARDOP_UI_DEVICESPAGE_H_
#define ARDOP_UI_DEVICESPAGE_H_

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

#include "modemthread.h"

/**
 * @file devicespage.h
 * @brief Choosing a sound card and a way to key the radio.
 *
 * This screen is why the platform layer exists.
 * [analysis/14](../../analysis/14-station-application.md) says the requirement
 * that made this an application rather than a daemon was "audio device selection
 * from the UI", and everything underneath -- the recoverable fault, the device
 * manager, the resolution rule, the settings store -- was built so that this
 * could be a screen rather than a restart.
 *
 * ## Two ways in, and the first one should usually work
 *
 * **Detected radios** comes first because it is the answer to the question
 * operators actually get stuck on: which of `ttyUSB0`, `ttyUSB1` and `ttyUSB2`
 * is the radio. In the common case a radio is one USB cable carrying both audio
 * and keying, so the pairing is knowable, and picking a detected entry fills in
 * all three fields at once.
 *
 * **The pickers** are underneath, always present, and are the whole interface on
 * a machine where detection finds nothing -- an onboard sound card with a
 * separate interface, or Windows, where the reader is not written yet.
 *
 * ## Nothing is applied by looking at it
 *
 * Choosing a detected radio fills the fields in. It does not open anything and
 * it does not key anything. **Apply** opens; **Test PTT** transmits for a second
 * and is the only control here that can put a signal on the air. That separation
 * is deliberate: CM108 and CI-V keying have no feedback path, so a human with a
 * receiver is the only confirmation that keying works, and a screen that keyed a
 * transmitter as a side effect of being browsed would be a bad way to find out.
 *
 * ## Enumeration blocks, and is therefore not on the modem thread
 *
 * `ardop_audio_enumerate` opens its own device context and queries every
 * device's native formats. On the modem thread that stall would trip the
 * backend's capture watchdog and manufacture a fault out of a device listing, so
 * it runs here -- on the interface thread, when the operator asks, accepting a
 * short pause on a screen they have just opened.
 */
class DevicesPage : public QWidget {
	Q_OBJECT

public:
	explicit DevicesPage(ModemThread *modem, QWidget *parent = nullptr);

	/** @brief Re-enumerate and re-detect. Blocks briefly; see the header. */
	void refresh();

	/** @brief The selection as the fields currently read it. */
	app_device_selection selection() const;

	/** @brief Fill the fields from @p sel, without applying it. */
	void setSelection(const app_device_selection &sel);

public slots:
	/** @brief Update the state line. Driven by the window's pump. */
	void updateStatus();

signals:
	/** @brief The operator applied a selection worth remembering. */
	void selectionApplied(const app_device_selection &sel);

private slots:
	void onPttMethodChanged(int index);
	void onDetectedChosen();
	void onApply();
	void onClose();
	void onTestPtt();

private:
	void fillDevices();
	void fillDetected();
	void fillPttTargets();

	ModemThread *m_modem = nullptr;

	QListWidget *m_detected = nullptr;
	QLabel *m_detectNote = nullptr;

	QComboBox *m_capture = nullptr;
	QComboBox *m_playback = nullptr;
	QComboBox *m_pttMethod = nullptr;
	QComboBox *m_pttTarget = nullptr;   /**< Editable: a list, not a cage. */
	QLabel *m_pttHint = nullptr;

	QPushButton *m_apply = nullptr;
	QPushButton *m_close = nullptr;
	QPushButton *m_test = nullptr;
	QLabel *m_state = nullptr;

	QString m_backend;   /**< Forced miniaudio backend, or empty. */
};

#endif /* ARDOP_UI_DEVICESPAGE_H_ */
