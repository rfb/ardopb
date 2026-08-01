#ifndef ARDOP_GUI_GAUGEWIDGET_H_
#define ARDOP_GUI_GAUGEWIDGET_H_

#include <QString>
#include <QWidget>

/**
 * @brief A semicircular needle gauge with a coloured arc and a caption.
 *
 * The VARA-style instrument: a needle over a green/yellow/red band, with the
 * numeric value spelled out underneath. A needle is read at a glance -- "is it
 * in the red" -- in a way a number is not, which is what you want while turning
 * a knob and listening.
 *
 * Reused for every instrument on the panel; the caller supplies the range, the
 * band boundaries and the caption text.
 */
class GaugeWidget : public QWidget {
	Q_OBJECT

public:
	explicit GaugeWidget(const QString &title, QWidget *parent = nullptr);

	/**
	 * @brief Set the scale and where the colour bands change.
	 *
	 * Bands run green from @p min to @p warn, yellow to @p bad, red beyond.
	 * For a scale where low is bad (S/N), pass @p warn > @p bad and the
	 * bands are laid out in reverse.
	 */
	void setScale(double min, double max, double warn, double bad);

	/** @brief Set the needle position and the caption under the dial. */
	void setValue(double value, const QString &caption);

	/** @brief Grey the gauge out and show a placeholder caption. */
	void setUnknown(const QString &caption);

protected:
	void paintEvent(QPaintEvent *) override;
	QSize minimumSizeHint() const override { return QSize(120, 104); }

private:
	QColor bandColour(double v) const;

	QString m_title;
	QString m_caption;
	double m_min = 0.0, m_max = 100.0;
	double m_warn = 60.0, m_bad = 80.0;
	double m_value = 0.0;
	bool m_known = false;
};

#endif /* ARDOP_GUI_GAUGEWIDGET_H_ */
