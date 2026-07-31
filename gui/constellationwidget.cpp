#include "constellationwidget.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

extern "C" {
#include "codec/frame.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @file constellationwidget.cpp
 * @brief The symbol-decision scatter (see constellationwidget.h).
 */

namespace {
/* Frames kept. Enough that a slow mode still shows a populated plot between
 * frames, few enough that a changing channel is visible as it changes. */
constexpr int kHistory = 3;

/* Rotation applied so 4PSK clusters land in the quadrants rather than on the
 * axes -- presentation only; see the header. */
constexpr double kRotate = M_PI / 4.0;

/* The modulation values, matching ardop_modulation in codec/frame.h. Repeated
 * rather than included so the GUI does not pull in the protocol headers; the
 * enum is normative and frozen, so this cannot drift. */
enum { kMod4FSK = 0, kMod4PSK = 1, kMod8PSK = 2, kMod16QAM = 3 };

const char *modName(quint8 m)
{
	switch (m) {
	case kMod4FSK:  return "4FSK";
	case kMod4PSK:  return "4PSK";
	case kMod8PSK:  return "8PSK";
	case kMod16QAM: return "16QAM";
	}
	return "?";
}

/*
 * The frame's mode name, from the modem's own table: "4PSK.200.100" for a data
 * frame (the same name FECMODE takes), or the control frame's name. Falls back
 * to the modulation family if the byte is not a known type.
 *
 * Worth more than the modulation alone: "16QAM" does not say how wide or how
 * fast, and bandwidth is exactly what an operator is deciding about.
 */
QString frameModeName(quint8 frameType, quint8 modulation)
{
	char buf[32];
	if (ardop_data_frame_name(frameType, buf, sizeof(buf)))
		return QString::fromLatin1(buf);

	const ardop_frame_spec *spec = ardop_frame_spec_for(frameType);
	if (spec && spec->name)
		return QString::fromLatin1(spec->name);

	return QString::fromLatin1(modName(modulation));
}
}   // namespace

ConstellationWidget::ConstellationWidget(QWidget *parent)
	: QWidget(parent)
{
	setMinimumSize(160, 160);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QString ConstellationWidget::modeName() const
{
	if (m_history.isEmpty())
		return QString();
	return frameModeName(m_frameType, m_modulation);
}

void ConstellationWidget::clear()
{
	m_history.clear();
	update();
}

void ConstellationWidget::setFrame(const ConstellationFrame &frame)
{
	m_modulation = frame.modulation;
	m_frameType = frame.frameType;
	m_history.append(frame);
	while (m_history.size() > kHistory)
		m_history.removeFirst();
	update();
}

/*
 * 4FSK: four horizontal lanes, one per tone, each symbol a point placed by how
 * far the winning tone beat the runner-up. There is no I/Q plane here -- the
 * mode decides on tone magnitude -- so plotting one would be a fiction. Points
 * hugging the right are confident decisions; a drift toward the left edge is a
 * frame about to fail, which is the same judgement the PSK scatter supports.
 */
void ConstellationWidget::paintFsk(QPainter &p)
{
	const ConstellationFrame &f = m_history.last();

	const int margin = 22;
	const QRect box = rect().adjusted(margin, margin + 10, -10, -18);
	if (box.width() <= 0 || box.height() <= 0)
		return;

	const int lanes = 4;
	const double laneH = box.height() / double(lanes);

	/* Lane guides and tone labels. */
	p.setPen(QPen(QColor(70, 130, 180), 1.0));
	for (int t = 0; t < lanes; t++) {
		const int y = int(box.top() + (t + 0.5) * laneH);
		p.drawLine(box.left(), y, box.right(), y);
	}
	p.setPen(QColor(150, 190, 190));
	for (int t = 0; t < lanes; t++) {
		const int y = int(box.top() + (t + 0.5) * laneH);
		p.drawText(QRect(2, y - 8, margin - 4, 16),
			   Qt::AlignRight | Qt::AlignVCenter,
			   QString::number(t));
	}

	/* A confidence gate at 25%: below this the winner barely beat the
	 * runner-up, which is where 4FSK errors come from. */
	const int gateX = int(box.left() + 0.25 * box.width());
	p.setPen(QPen(QColor(200, 140, 90), 1.0, Qt::DashLine));
	p.drawLine(gateX, box.top(), gateX, box.bottom());

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(120, 255, 120, 200));
	const int n = int(std::min(f.phaseMrad.size(), f.mag.size()));
	for (int i = 0; i < n; i++) {
		const int tone = std::clamp(int(f.phaseMrad[i]), 0, lanes - 1);
		const double conf = std::clamp(f.mag[i] / 1000.0, 0.0, 1.0);
		/* Spread within the lane so overlapping symbols stay visible. */
		const double jitter = ((i * 37) % 11 - 5) / 5.0 * (laneH * 0.16);
		/* Inset slightly so a perfect-margin symbol sits inside the
		 * plot rather than half-clipped on its edge. */
		const QPointF pt(box.left() + conf * (box.width() - 4),
				 box.top() + (tone + 0.5) * laneH + jitter);
		p.drawEllipse(pt, 1.6, 1.6);
	}

	p.setPen(QColor(150, 190, 190));
	p.drawText(QRect(box.left(), box.bottom() + 2, box.width(), 16),
		   Qt::AlignLeft, tr("decision margin →"));
}

void ConstellationWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	/* VARA's teal, which has become the convention for this plot. */
	const QColor bg(0, 78, 84);
	p.fillRect(rect(), bg);

	auto label = [&](const QString &extra) {
		const QString text = QStringLiteral("%1   0x%2   %3")
					     .arg(frameModeName(m_frameType,
								m_modulation))
					     .arg(m_frameType, 2, 16,
						  QLatin1Char('0'))
					     .arg(extra);
		/* A backing strip: mode names are long enough to run under the
		 * outer clusters, and the plot is drawn first. */
		const int h = p.fontMetrics().height() + 4;
		p.fillRect(QRect(0, 0, width(), h), QColor(0, 52, 57, 205));
		p.setPen(QColor(215, 235, 235));
		p.drawText(QRect(6, 0, width() - 12, h),
			   Qt::AlignVCenter | Qt::AlignLeft, text);
	};

	if (m_history.isEmpty()) {
		p.setPen(QColor(150, 180, 180));
		p.drawText(rect(), Qt::AlignCenter, tr("waiting for a frame"));
		return;
	}

	if (m_modulation == kMod4FSK) {
		if (m_history.last().phaseMrad.isEmpty()) {
			label(tr("no symbols"));
			return;
		}
		paintFsk(p);
		label(tr("%1 symbols").arg(m_history.last().phaseMrad.size()));
		return;
	}

	if (m_history.last().phaseMrad.isEmpty()) {
		/* A control frame: no payload symbols to slice. */
		label(tr("no symbols"));
		return;
	}

	const int side = std::min(width(), height());
	const QPointF centre(width() / 2.0, height() / 2.0);
	const double radius = side / 2.0 - 6.0;
	if (radius <= 0.0)
		return;

	/* --- decision boundaries ---------------------------------------- */
	p.setPen(QPen(QColor(70, 130, 180), 1.0));

	auto ray = [&](double angle) {
		p.drawLine(centre,
			   centre + QPointF(radius * std::cos(angle),
					    -radius * std::sin(angle)));
	};

	switch (m_modulation) {
	case kMod4PSK:
		/* Slicer thresholds at +/-pi/4 and +/-3pi/4; after the pi/4
		 * display rotation these fall on the axes. */
		for (int k = 0; k < 4; k++)
			ray(k * M_PI / 2.0);
		break;
	case kMod8PSK:
		/* Thresholds every pi/4, offset by pi/8. */
		for (int k = 0; k < 8; k++)
			ray(k * M_PI / 4.0 + M_PI / 8.0 + kRotate);
		break;
	case kMod16QAM:
		/* Same eight phase sectors as 8PSK; the fourth bit comes from
		 * an amplitude decision, so the ring is drawn below once the
		 * magnitude scale is known. */
		for (int k = 0; k < 8; k++)
			ray(k * M_PI / 4.0 + M_PI / 8.0 + kRotate);
		break;
	default:
		break;
	}

	/* --- the points --------------------------------------------------- */
	/* Scale so the strongest symbol of the newest frame reaches the edge:
	 * magnitudes are in demodulator units whose absolute value depends on
	 * audio gain, so only the *shape* is meaningful. */
	double maxMag = 1.0;
	for (qint16 m : m_history.last().mag)
		maxMag = std::max(maxMag, double(m));

	/* The 16QAM inner/outer boundary, at the decoder's own adaptive
	 * threshold scaled the same way the points are. Drawn under them. */
	if (m_modulation == kMod16QAM && m_history.last().magThreshold > 0) {
		const double rr = (m_history.last().magThreshold / maxMag)
				  * radius;
		p.setPen(QPen(QColor(70, 130, 180), 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(centre, rr, rr);
	}

	for (int h = 0; h < m_history.size(); h++) {
		const ConstellationFrame &f = m_history[h];
		/* Newest brightest; older frames fade into the background. */
		const int age = m_history.size() - 1 - h;
		const int alpha = age == 0 ? 235 : std::max(40, 150 - age * 55);
		const double dotR = age == 0 ? 1.7 : 1.3;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(120, 255, 120, alpha));

		const int n = int(std::min(f.phaseMrad.size(), f.mag.size()));
		for (int i = 0; i < n; i++) {
			const double a = f.phaseMrad[i] / 1000.0 + kRotate;
			const double r = (f.mag[i] / maxMag) * radius;
			const QPointF pt(centre.x() + r * std::cos(a),
					 centre.y() - r * std::sin(a));
			p.drawEllipse(pt, dotR, dotR);
		}
	}

	label(tr("%1 pts").arg(m_history.last().phaseMrad.size()));
}
