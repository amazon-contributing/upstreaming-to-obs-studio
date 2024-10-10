
#pragma once

#include <obs.hpp>
#include <QObject>
#include <QThread>

#include <chrono>
#include <optional>

class SubmissionWorker : public QObject {
	Q_OBJECT;

private:
	QString url_;
	std::vector<OBSData> pending_events_;
	std::optional<std::chrono::steady_clock::time_point> last_send_time_;

public:
	SubmissionWorker(QString url) : url_(url)
	{
		connect(this, &SubmissionWorker::PendingEvent, this, &SubmissionWorker::AttemptSubmission,
			Qt::QueuedConnection);
	}

	~SubmissionWorker();

public slots:
	void QueueEvent(OBSData event);

private slots:
	void AttemptSubmission();

signals:
	void PendingEvent();
	void SubmissionError(OBSData error);
};

class BerryessaSubmitter : public QObject {
	Q_OBJECT

public:
	BerryessaSubmitter(QObject *parent, QString url);
	~BerryessaSubmitter();

	/**
	 * Submit an event to be sent to Berryessa. This takes
	 * ownership of `properties`.
	 *
	 * XXX: this will eventually put items on a queue which another
	 * thread will send asynchronously. Right now it's synchronous and
	 * blocking.
	 */
	void submit(QString eventName, obs_data_t *properties);

	/**
	 * This property key/value will be added to every item submitted by this
	 * BerryessaSubmitter, unless overridden by a subsequent setAlwaysBool or unsetAlways.
	 */
	void setAlwaysBool(QString propertyKey, bool propertyValue);

	/**
	 * This property key/value will be added to every item submitted by this
	 * BerryessaSubmitter, unless overridden by a subsequent setAlwaysString or unsetAlways.
	 */
	void setAlwaysString(QString propertyKey, QString propertyValue);

	/**
	 * Undoes setAlwaysBool and setAlwaysString.
	 */
	void unsetAlways(QString propertyKey);

public slots:
	void SubmissionError(OBSData error);

signals:
	void SubmitEvent(OBSData event);

private:
	QString url_;
	OBSDataAutoRelease alwaysProperties_;

	SubmissionWorker submission_worker_;
	QThread submission_thread_;
};
