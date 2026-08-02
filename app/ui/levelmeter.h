#ifndef ARDOP_UI_LEVELMETER_H_
#define ARDOP_UI_LEVELMETER_H_

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

/**
 * @file levelmeter.h
 * @brief A horizontal level meter with a following peak indicator.
 *
 * [analysis/16](../../analysis/16-user-interface.md) §9. Replaces the two
 * semicircular dials on the panel, for two reasons -- one about space and one
 * about correctness.
 *
 * **Space.** A needle dial spends most of its area on the arc and the bezel and
 * encodes one number, which is more legible as a number. Two of them cost around
 * 200 px of the height the waterfall wants.
 *
 * **Correctness, which is the real one.** A dial coloured green below a single
 * threshold says a signal 40 dB too quiet is fine. It is not fine -- it is the
 * commonest real fault on a receive path, and the display an operator is looking
 * at while it happens actively reassures them. So this meter is built around a
 * *band* rather than a threshold: below it is as wrong as above it, and looks it.
 *
 * ## The ballistics run on a repaint timer, not on setValue
 *
 * ::setValue only records the raw figures; a 30 Hz timer advances what is drawn,
 * by elapsed milliseconds.
 *
 * That is not indirection for its own sake. An audio record arrives once per
 * captured block, and the block size belongs to the backend: 300 samples at
 * 48 kHz is 40 Hz, and the 1200-sample default is 10 Hz. A decay applied per
 * record would fall four times faster on one sound card than on another -- the
 * meter would behave differently on two machines watching the same signal, which
 * is exactly what an instrument may not do.
 *
 * ## One widget, two very different scales
 *
 * Audio level is two-sided: too quiet and too loud are both faults. S/N is
 * one-sided -- more is simply better -- which is expressed by putting the band's
 * top edge at the top of the scale rather than by a second class.
 */
class LevelMeter : public QWidget {
	Q_OBJECT

public:
	explicit LevelMeter(const QString &label, QWidget *parent = nullptr);

	/** @brief The scale, in whatever unit the caller is measuring. */
	void setScale(double min, double max);

	/**
	 * @brief Where the value is supposed to sit.
	 *
	 * Drawn behind the bar, and what the colour is decided against. Values
	 * below @p lo read as "too quiet" rather than "fine", which is the defect
	 * this widget exists to fix.
	 */
	void setBand(double lo, double hi);

	/** @brief New data. @p peak may equal @p level for a one-sided reading. */
	void setValue(double level, double peak);

	/** @brief No data. Shows @p caption and an empty trough. */
	void setUnknown(const QString &caption);

	/** @brief The trailing text, e.g. "-16.2 dB  pk -9.4". */
	void setCaption(const QString &caption);

	/* Tall enough for its three bands: the scale numbers, the trough, and the
	 * label with its reading. §9 said 34; that was measured against a design
	 * and not against text, and clipped the numbers. */
	QSize minimumSizeHint() const override { return QSize(220, 43); }
	QSize sizeHint() const override { return QSize(420, 43); }

	/* --- observation, for app/ui/test_widgets.cpp ------------------------ */

	/** @brief The displayed level, after ballistics. */
	double displayedLevel() const { return m_level; }
	/** @brief The displayed peak, after hold and fall. */
	double displayedPeak() const { return m_peak; }
	/** @brief Advance the ballistics by @p ms, instead of by the clock. */
	void advanceForTest(qint64 ms);

protected:
	void paintEvent(QPaintEvent *) override;

private slots:
	void tick();

private:
	int toX(double v) const;      /* value -> pixel, clamped to the scale */
	QColor barColour() const;
	void step(double dt);

	QString m_label, m_caption;
	double m_min = -60.0, m_max = 0.0;
	double m_bandLo = -24.0, m_bandHi = -12.0;

	/* Displayed, after ballistics. Seeded far below any scale so the first
	 * reading attacks instantly rather than rising from a made-up value. */
	double m_level = -1e9, m_peak = -1e9;
	double m_rawLevel = -1e9, m_rawPeak = -1e9;

	QElapsedTimer m_clock;
	qint64 m_lastTickMs = 0, m_peakHoldUntilMs = 0;
	QTimer m_repaint;
	bool m_haveData = false;
};

#endif /* ARDOP_UI_LEVELMETER_H_ */
