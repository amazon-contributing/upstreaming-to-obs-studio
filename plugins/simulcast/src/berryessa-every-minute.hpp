
#pragma once

#include <QObject>
#include <QDateTime>

class BerryessaSubmitter;
class PresentMonCapture;
class QTimer;

class BerryessaEveryMinute : public QObject {
	Q_OBJECT
public:
	BerryessaEveryMinute(QObject *parent, BerryessaSubmitter *berryessa);
	virtual ~BerryessaEveryMinute();

private slots:
	void fire();

private:
	BerryessaSubmitter *berryessa_;
	PresentMonCapture* presentmon_;
	QTimer* timer_;
	QDateTime startTime_;
};
