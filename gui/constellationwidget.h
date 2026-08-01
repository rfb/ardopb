#ifndef ARDOP_GUI_CONSTELLATIONWIDGET_H_
#define ARDOP_GUI_CONSTELLATIONWIDGET_H_

#include <QString>
#include <QVector>
#include <QWidget>

#include "telemetryclient.h"

/**
 * @brief Where the demodulator's symbol decisions are landing.
 *
 * Each point is one demodulated symbol, plotted from the *differential phase
 * and magnitude the decoder itself slices* -- not a re-derivation. So this is
 * literally the input to the bit decision, and the overlay drawn across it is
 * the decoder's own threshold, taken from ardop_decode_psk_char:
 *
 *   4PSK  slices at +/-786 and +/-2356 mrad  (+/-pi/4, +/-3pi/4)
 *   8PSK  slices at +/-393 mrad              (+/-pi/8)
 *
 * Tight clusters well inside their sectors mean confident decisions; points
 * smeared across a boundary are bits about to be wrong. Cluster count is the
 * modulation order: 4, 8, or a 4x4 grid for 16QAM.
 *
 * The plot is rotated by pi/4 so the four 4PSK clusters sit in the quadrants
 * with the boundaries on the axes. ARDOP's clusters are actually *on* the axes
 * with the boundaries diagonal; rotating gives the familiar four-quadrant
 * picture every other modem shows, at no cost to what is being conveyed.
 *
 * Points fade over successive frames rather than clearing, so a display that
 * updates once per frame still shows the trend rather than flickering.
 */
class ConstellationWidget : public QWidget {
	Q_OBJECT

public:
	explicit ConstellationWidget(QWidget *parent = nullptr);

	/** @brief Mode name of the last frame ("4PSK.200.100"), or empty. */
	QString modeName() const;

public slots:
	void setFrame(const ConstellationFrame &frame);
	void clear();

protected:
	void paintEvent(QPaintEvent *) override;

private:
	void paintFsk(QPainter &p);

private:
	/* Recent frames, newest last; older ones are drawn dimmer. */
	QVector<ConstellationFrame> m_history;
	quint8 m_modulation = 0;
	quint8 m_frameType = 0;
};

#endif /* ARDOP_GUI_CONSTELLATIONWIDGET_H_ */
