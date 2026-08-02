#ifndef ARDOP_GUI_WATERFALLWIDGET_H_
#define ARDOP_GUI_WATERFALLWIDGET_H_

#include <QImage>
#include <QVector>
#include <QWidget>

#include "telemetryclient.h"

/**
 * @brief A scrolling spectrogram of the receiver's passband.
 *
 * One image row per spectrum record (~11.7/s at 12 kHz / 1024). New rows enter
 * at the top and the image scrolls down, matching every other waterfall an
 * operator has used.
 *
 * Levels are auto-ranged rather than fixed: the absolute magnitudes depend on
 * the sound card's gain staging, which the modem cannot know, so a fixed scale
 * would be blank on one radio and saturated on the next. The noise floor is
 * tracked slowly and the display spans a fixed dB window above it.
 */
class WaterfallWidget : public QWidget {
	Q_OBJECT

public:
	explicit WaterfallWidget(QWidget *parent = nullptr);

	/** @brief Set the bin geometry used to label the frequency axis. */
	void setSpectrumGeometry(int bins, int firstBin, float binHz);

	/**
	 * @brief Rows the history image holds, in device pixels.
	 *
	 * Observation only, for the scale test in app/ui/test_widgets.cpp. The
	 * unit is the whole point: this must cover the canvas in *device* pixels,
	 * not logical ones, or the vertical blit is fractional and the display
	 * shimmers on any screen not at 100%.
	 */
	int historyRows() const { return m_img.height(); }

public slots:
	void addRow(const SpectrumRow &row);
	void clear();

protected:
	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *) override;

private:
	QRgb colourFor(float t) const;
	int historyHeight() const;
	int canvasHeight() const;   /* logical pixels: layout and the axis. */
	int canvasRows() const;     /* device pixels: the image and the blit. */
	void ensureHeight(int rows);

	QImage m_img;            /* bins wide, history tall; scaled on paint.
	                          * Rows are device pixels, not logical ones --
	                          * see ensureHeight. */
	int m_bins = ARDOP_BUSY_MAG_BINS;
	int m_firstBin = ARDOP_BUSY_FIRST_BIN;
	float m_binHz = ARDOP_BUSY_BIN_HZ;

	float m_floorDb = 0.0f;  /* slow-tracked noise floor. */
	bool m_haveFloor = false;
	int m_rows = 0;          /* rows written, so we can skip empty paint. */
};

#endif /* ARDOP_GUI_WATERFALLWIDGET_H_ */
