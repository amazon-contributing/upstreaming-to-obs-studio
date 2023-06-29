
#pragma once

#include <obs.hpp>
#include <QObject>

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
	 * BerryessaSubmitter, unless overridden by a subsequent setAlwaysString or unsetAlways.
	 */
	void setAlwaysString(QString propertyKey, QString propertyValue);

	/**
	 * Undoes setAlwaysString.
	 */
	void unsetAlways(QString propertyKey);

private:
	QString url_;
	OBSDataAutoRelease alwaysProperties_;

	/**
	 * Attempts to submit the passed items to Berryessa in a single HTTP request.
	 * May retry once or more. (XXX: does not currently retry)
	 * Calls obs_data_release() on passed items before returning.
	 *
	 * On success: returns NULL.
	 * On failure: returns k-v items describing the error, suitable
	 *          for submission as an ivs_obs_http_client_error event :)
	 */
	obs_data_t *syncSubmitAndReleaseItemsReturningError(
		const std::vector<obs_data_t *> &items);
};
