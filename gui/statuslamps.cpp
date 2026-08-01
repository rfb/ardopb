#include "statuslamps.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>

/**
 * @file statuslamps.cpp
 * @brief The link-state lamp panel (see statuslamps.h).
 */

namespace {
/*
 * ardop_link_state, in declaration order. Repeated rather than included so the
 * GUI stays free of the protocol headers; if a state is ever added, an unknown
 * index shows as "?" rather than mislabelling a neighbour.
 */
const char *const kStates[] = {
	"DISC", "ISS_CON_REQ", "ISS_CON_ACK", "ISS",
	"IRS_CON_ACK", "IRS_DATA", "IRS_FROM_ISS", "IDLE",
	"IRS_TO_ISS", "FECSEND",
};
constexpr int kNumStates = int(sizeof(kStates) / sizeof(kStates[0]));

/* The five an operator actually watches for, as lamps. */
const char *const kLamps[] = {"DISC", "ISS", "IRS", "IDLE", "FECSEND"};
constexpr int kNumLamps = int(sizeof(kLamps) / sizeof(kLamps[0]));

const char *const kModes[] = {"ARQ", "FEC", "RXO"};

/* Which lamp a raw state lights. Several protocol states collapse onto one
 * lamp: an operator cares that we are the sending station, not which of the
 * three ISS sub-states the machine is in. */
int lampFor(quint8 state)
{
	switch (state) {
	case 0: return 0;                    /* DISC */
	case 1: case 2: case 3: return 1;    /* ISS* */
	case 4: case 5: case 6: return 2;    /* IRS* */
	case 7: return 3;                    /* IDLE */
	case 8: return 2;                    /* IRS_TO_ISS: still IRS side */
	case 9: return 4;                    /* FECSEND */
	}
	return -1;
}

void drawLamp(QPainter &p, const QRect &r, const QString &text, bool on,
	      const QColor &colour)
{
	const int d = std::min(12, r.height() - 4);
	QRect dot(r.left() + 2, r.center().y() - d / 2, d, d);

	p.setPen(QPen(QColor(90, 90, 90), 1.0));
	p.setBrush(on ? colour : QColor(60, 60, 60));
	p.drawEllipse(dot);

	p.setPen(QColor(40, 40, 40));
	p.drawText(r.adjusted(d + 8, 0, 0, 0),
		   Qt::AlignVCenter | Qt::AlignLeft, text);
}
}   // namespace

StatusLamps::StatusLamps(QWidget *parent)
	: QWidget(parent)
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	setMinimumHeight(104);
}

void StatusLamps::setStatus(const LinkStatus &st)
{
	m_st = st;
	m_online = true;
	update();
}

void StatusLamps::setOffline()
{
	m_online = false;
	update();
}

void StatusLamps::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	QFont f = p.font();
	f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
	p.setFont(f);

	const int rowH = std::max(13, (height() - 4) / 5);
	const int colW = width() / 2;
	const int lit = m_online ? lampFor(m_st.state) : -1;

	/* Left column: the protocol state lamps. */
	for (int i = 0; i < kNumLamps && i < 4; i++) {
		QRect r(2, 2 + i * rowH, colW - 4, rowH);
		drawLamp(p, r, QString::fromLatin1(kLamps[i]), i == lit,
			 QColor(80, 210, 90));
	}

	/* Right column: FECSEND, then the two that matter while operating. */
	QRect r0(colW, 2, colW - 4, rowH);
	drawLamp(p, r0, QString::fromLatin1(kLamps[4]), lit == 4,
		 QColor(80, 210, 90));

	QRect r1(colW, 2 + rowH, colW - 4, rowH);
	drawLamp(p, r1, QStringLiteral("BUSY"), m_online && m_st.busy,
		 QColor(235, 200, 60));

	QRect r2(colW, 2 + 2 * rowH, colW - 4, rowH);
	drawLamp(p, r2, QStringLiteral("PTT"), m_online && m_st.ptt,
		 QColor(225, 70, 60));

	/* Bottom line: mode, negotiated width, queued bytes, raw state name. */
	QRect info(2, 2 + 4 * rowH, width() - 4, rowH);
	p.setPen(QColor(70, 70, 70));
	QString text;
	if (!m_online) {
		text = tr("offline");
	} else {
		const char *mode = m_st.mode < 3 ? kModes[m_st.mode] : "?";
		const char *state = m_st.state < kNumStates
					    ? kStates[m_st.state] : "?";
		text = QStringLiteral("%1 · %2").arg(mode).arg(state);
		if (m_st.bandwidth > 0)
			text += QStringLiteral(" · %1 Hz").arg(m_st.bandwidth);
		if (m_st.bufferLen > 0)
			text += tr(" · %1 B queued").arg(m_st.bufferLen);
	}
	p.drawText(info, Qt::AlignVCenter | Qt::AlignLeft, text);
}
