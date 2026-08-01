#include "gaugewidget.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @file gaugewidget.cpp
 * @brief The needle instrument (see gaugewidget.h).
 */

namespace {
/* The dial sweeps 180 degrees, drawn in Qt's 1/16-degree arc units. */
constexpr int kStartAngle = 180 * 16;
constexpr int kSweep = -180 * 16;

const QColor kGreen(60, 190, 80);
const QColor kYellow(225, 200, 60);
const QColor kRed(215, 70, 60);
}   // namespace

GaugeWidget::GaugeWidget(const QString &title, QWidget *parent)
	: QWidget(parent), m_title(title)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	setMinimumHeight(104);
}

void GaugeWidget::setScale(double min, double max, double warn, double bad)
{
	m_min = min;
	m_max = max;
	m_warn = warn;
	m_bad = bad;
	update();
}

void GaugeWidget::setValue(double value, const QString &caption)
{
	m_value = std::clamp(value, std::min(m_min, m_max),
			     std::max(m_min, m_max));
	m_caption = caption;
	m_known = true;
	update();
}

void GaugeWidget::setUnknown(const QString &caption)
{
	m_caption = caption;
	m_known = false;
	update();
}

/* Which band a value falls in. When warn > bad the scale is reversed (low is
 * bad), which is how S/N reads. */
QColor GaugeWidget::bandColour(double v) const
{
	if (m_warn <= m_bad)
		return v < m_warn ? kGreen : (v < m_bad ? kYellow : kRed);
	return v > m_warn ? kGreen : (v > m_bad ? kYellow : kRed);
}

void GaugeWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const int capH = 16;
	const int titleH = 14;
	QRect dial(4, 2, width() - 8, height() - capH - 2);
	const int d = std::min(dial.width(), dial.height() * 2);
	QRect arcBox(dial.center().x() - d / 2, dial.top(), d, d);

	/* --- the coloured band ------------------------------------------- */
	const double span = m_max - m_min;
	if (std::abs(span) > 1e-9) {
		QPen pen;
		pen.setWidthF(9.0);
		pen.setCapStyle(Qt::FlatCap);

		/* Walk the sweep in steps, colouring by the value each step
		 * represents. Simpler than computing three arc extents, and it
		 * handles a reversed scale without a special case. */
		constexpr int kSteps = 60;
		for (int i = 0; i < kSteps; i++) {
			double t0 = double(i) / kSteps;
			double v = m_min + t0 * span;
			pen.setColor(bandColour(v));
			p.setPen(pen);
			p.drawArc(arcBox.adjusted(6, 6, -6, -6),
				  kStartAngle + int(kSweep * t0),
				  kSweep / kSteps - 1);
		}
	}

	/* --- the needle --------------------------------------------------- */
	const QPointF pivot(arcBox.center().x(), arcBox.center().y());
	if (m_known && std::abs(span) > 1e-9) {
		const double t = (m_value - m_min) / span;
		const double angle = M_PI * (1.0 - std::clamp(t, 0.0, 1.0));
		const double len = arcBox.width() / 2.0 - 12.0;

		p.setPen(QPen(QColor(30, 30, 30), 2.0));
		p.drawLine(pivot, pivot + QPointF(len * std::cos(angle),
						  -len * std::sin(angle)));
		p.setBrush(QColor(40, 70, 140));
		p.setPen(Qt::NoPen);
		p.drawEllipse(pivot, 4.0, 4.0);
	} else {
		p.setBrush(QColor(140, 140, 140));
		p.setPen(Qt::NoPen);
		p.drawEllipse(pivot, 4.0, 4.0);
	}

	/* --- title and caption -------------------------------------------- */
	p.setPen(palette().color(QPalette::WindowText));
	QFont bold = p.font();
	bold.setBold(true);
	p.setFont(bold);
	p.drawText(QRect(0, int(pivot.y()) - titleH, width(), titleH),
		   Qt::AlignHCenter | Qt::AlignVCenter, m_title);

	QFont small = p.font();
	small.setBold(false);
	small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
	p.setFont(small);

	QRect capRect(2, height() - capH, width() - 4, capH - 1);
	p.fillRect(capRect, m_known ? bandColour(m_value).lighter(150)
				    : QColor(200, 200, 200));
	p.setPen(QColor(20, 20, 20));
	/* Elide rather than overflow: these captions carry two figures and the
	 * gauge is narrow when several share a row. */
	const QString shown = p.fontMetrics().elidedText(
		m_caption, Qt::ElideRight, capRect.width() - 4);
	p.drawText(capRect, Qt::AlignCenter, shown);
}
