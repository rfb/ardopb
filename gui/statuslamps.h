#ifndef ARDOP_GUI_STATUSLAMPS_H_
#define ARDOP_GUI_STATUSLAMPS_H_

#include <QLabel>
#include <QVector>
#include <QWidget>

#include "telemetryclient.h"

/**
 * @brief The link-state lamp panel: which protocol state, busy, PTT.
 *
 * VARA lights a grid of DATA/ACK/NACK/REQ/IDLE/BREAK/QRT. ARDOP's states are
 * not the same set, so this lights ARDOP's own -- DISC / ISS / IRS / IDLE /
 * FECSEND -- rather than mapping them onto labels that would mean something
 * else. The two that matter most while operating, BUSY and PTT, get their own
 * prominent lamps.
 */
class StatusLamps : public QWidget {
	Q_OBJECT

public:
	explicit StatusLamps(QWidget *parent = nullptr);

public slots:
	void setStatus(const LinkStatus &st);
	void setOffline();

protected:
	void paintEvent(QPaintEvent *) override;
	QSize minimumSizeHint() const override { return QSize(210, 104); }

private:
	LinkStatus m_st;
	bool m_online = false;
};

#endif /* ARDOP_GUI_STATUSLAMPS_H_ */
