
#pragma once

#include <obs.hpp>
#include <QObject>

class BerryessaSubmitter : public QObject {
	Q_OBJECT

public:
	BerryessaSubmitter(QObject* parent, QString url);
	~BerryessaSubmitter();

	/**
	 * Submit an event to be sent to Berryessa. This takes
	 * ownership of `properties`.
	 */
	void submit(QString eventName, obs_data_t *properties);

private:
	QString url_;

	/**
	 * Attempts to submit the passed items to Berryessa in a single HTTP request.
	 * May retry once or more.
	 * Calls obs_data_release() on passed items before returning.
	 *
	 * On success: returns NULL.
	 * On failure: returns k-v items describing the error, suitable
	 *          for submission as an ivs_obs_http_client_error event :)
	 */
	obs_data_t *syncSubmitAndReleaseItemsReturningError(
		const std::vector<obs_data_t*> &items);
};

