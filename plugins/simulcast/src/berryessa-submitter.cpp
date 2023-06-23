
#include "berryessa-submitter.hpp"

#include "copy-from-obs/remote-text.hpp"


//struct BerryessaSubmitterItem {
//	QString eventName;
//	obs_data_t *properties;
//};//

BerryessaSubmitter::BerryessaSubmitter(QObject *parent, QString url)
	: QObject(parent), url_(url)
{}

BerryessaSubmitter::~BerryessaSubmitter() {}

void BerryessaSubmitter::submit(QString eventName, obs_data_t* properties) {
	//BerryessaSubmitterItem item{eventName, properties};

	// XXX do the whole async worker thread thing
	//std::vector<BerryessaSubmitterItem> tmp;
	//tmp.push_back(item);

	obs_data_t *toplevel = obs_data_create();
	obs_data_set_string(toplevel, "Name", eventName.toUtf8());
	obs_data_set_obj(toplevel, "Properties", properties);

	std::vector<obs_data_t *> tmp;
	tmp.push_back(toplevel);
	obs_data_t *error = syncSubmitAndReleaseItemsReturningError(tmp);

	// not until we have async / queueing :)
	//if (error) {
	//  submit("ivs_obs_http_client_error", error);
	//}
}

obs_data_t* BerryessaSubmitter::syncSubmitAndReleaseItemsReturningError(
	const std::vector<obs_data_t*> &items)
{
	// Berryessa documentation:
	// https://docs.google.com/document/d/1dB1fOgGQxu05ljqVVoX1jcjImzlcwcm9QKFZ2IDeuo0/edit#heading=h.yjke1ko59g7n
	
	// build up json, obs_data_release'ing as we go
	// 
	QByteArray postJson;
	for (obs_data_t *it : items) {
		postJson += postJson.isEmpty() ? "[" : ",";
		postJson += obs_data_get_json(it);
		obs_data_release(it);
	}
	postJson += "]";

	// base64 encoding for HTTP post
	QByteArray postEncoded("data=");
	postEncoded += postJson.toBase64();

	// http post to berryessa
	std::vector<std::string> headers;
	headers.push_back(
		"User-Agent: obs-simulcast-plugin-tech-preview");
	headers.push_back(
		"Content-Type: application/x-www-form-urlencoded; charset=UTF-8");

	std::string httpResponse, httpError;
	long httpResponseCode;

	bool ok = GetRemoteFile(
		url_.toUtf8(),
		httpResponse, httpError, // out params
		&httpResponseCode, nullptr, // out params (response code and content type)
		"POST", postEncoded.constData(), headers,
		nullptr, // signature
		5);      // timeout in seconds

	// log and return http error information, if any
	// XXX parse response from berryessa, check response code?
	obs_data_t *error = nullptr;
	if (!ok) {
		error = obs_data_create();
		obs_data_set_string(error, "url", url_.toUtf8());
		obs_data_set_string(error, "error", httpError.c_str());
		obs_data_set_int(error, "response_code", httpResponseCode);
		blog(LOG_WARNING,
		     "Could not submit %d bytes to metrics backend: %s",
		     postEncoded.size(), obs_data_get_json(error));
	}
	return error;
}

