#include "simulcast-dock-widget.h"

#include "simulcast-output.h"

#include <QMainWindow>
#include <QDockWidget>
#include <QWidget>
#include <QLabel>
#include <QString>
#include <QPushButton>
#include <QScrollArea>
#include <QGridLayout>
#include <QEvent>
#include <QThread>
#include <QLineEdit>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QAction>
#include <QUuid>
#include <qprocess.h>

#include <obs.hpp>
#include <obs-module.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>

void afTest();

#define ConfigSection "simulcast"

#include "goliveapi-network.hpp"
#include "goliveapi-postdata.hpp"
#include "berryessa-submitter.hpp"

obs_data_t* MakeEvent_ivs_obs_stream_start(obs_data_t* postData, obs_data_t* goLiveConfig)
{
	obs_data_t *event = obs_data_create();

	// include the entire capabilities API request/response
	obs_data_set_string(event, "capabilities_api_request",
			    obs_data_get_json(postData));

	obs_data_set_string(event, "capabilities_api_response",
			    obs_data_get_json(goLiveConfig));

	// extract specific items of interest from the capabilities API response
	obs_data_t *goLiveMeta = obs_data_get_obj(goLiveConfig, "meta");
	if (goLiveMeta) {
		const char *s = obs_data_get_string(goLiveMeta, "config_id");
		if (s && *s) {
			obs_data_set_string(event, "config_id", s);
		}
	}

	obs_data_array_t *goLiveEncoderConfigurations =
		obs_data_get_array(goLiveConfig, "encoder_configurations");
	if (goLiveEncoderConfigurations) {
		obs_data_set_int(
			event, "encoder_count",
			obs_data_array_count(goLiveEncoderConfigurations));
	}

	return event;
}


SimulcastDockWidget::SimulcastDockWidget(QWidget *parent)
{
	//berryessa_ = new BerryessaSubmitter(this, "http://127.0.0.1:8787/");
	berryessa_ = new BerryessaSubmitter(this, "https://data-staging.stats.live-video.net/");

	// XXX: should be created once per device and persisted on disk
	berryessa_->setAlwaysString("device_id",
				     "bf655dd3-8346-4c1c-a7c8-bb7d9ca6091a");

	berryessa_->setAlwaysString("obs_session_id",
				    QUuid::createUuid().toString(QUuid::WithoutBraces));

	QGridLayout *dockLayout = new QGridLayout(this);
	dockLayout->setAlignment(Qt::AlignmentFlag::AlignTop);

	// start all, stop all
	auto buttonContainer = new QWidget(this);
	auto buttonLayout = new QVBoxLayout();
	auto streamingButton = new QPushButton(
		obs_module_text("Btn.StartStreaming"), buttonContainer);
	buttonLayout->addWidget(streamingButton);
	buttonContainer->setLayout(buttonLayout);
	dockLayout->addWidget(buttonContainer);

	QObject::connect(
		streamingButton, &QPushButton::clicked,
		[this, streamingButton]() {
			if (this->Output().IsStreaming()) {
				this->Output().StopStreaming();
				obs_data_t *event = obs_data_create();
				obs_data_set_string(event, "client_error", "");
				obs_data_set_string(event, "server_error", "");
				this->berryessa_->submit("ivs_obs_stream_stop",
							 event);

				this->berryessa_->unsetAlways("config_id");

				streamingButton->setText(
					obs_module_text("Btn.StartStreaming"));


			} else {
				OBSDataAutoRelease postData =
					constructGoLivePost();
				obs_data_t *goLiveConfig =
					DownloadGoLiveConfig(this, localConfig_.goLiveApiUrl, postData);

				if (goLiveConfig) {
					this->Output().StartStreaming(
						goLiveConfig);

					obs_data_t *event =
						MakeEvent_ivs_obs_stream_start(
							postData, goLiveConfig);
					const char* configId = obs_data_get_string(
							event, "config_id");
					if (configId)
					{
						// put the config_id on all events until the stream ends
						this->berryessa_
							->setAlwaysString(
								"config_id", configId);
					}
					this->berryessa_->submit(
						"ivs_obs_stream_start", event);

					afTest(); // XXX

					streamingButton->setText(
						obs_module_text(
							"Btn.StopStreaming"));
				}
			}
		});

	// load config
	LoadConfig();

	
#if 0
	// frame stuff
	// XXX where to put this?
	QProcess* process = new QProcess(this);
	bool alreadyErrored = false;
	QObject::connect(process, &QProcess::readyReadStandardOutput,
			 [process]() {
				 char buf[1024];
				if (!alreadyErrored) {
					while (process.canReadLine()) {
						 qint64 n = process->readLine(
							 buf, sizeof(buf));
						 // XXX emit error and stop reading if too long
						 if (n >= sizeof(buf) - 1) {
							//emit FramePipeline
						 }
					 }
				 }
			 });
#endif

	setLayout(dockLayout);

	resize(200, 400);
}

void SimulcastDockWidget::SaveConfig() {}

void SimulcastDockWidget::LoadConfig()
{
	auto profile_config = obs_frontend_get_profile_config();
	// XXX don't understand scope of this and the debugger doesn't know what's behind config_t*

	//localConfig_.goLiveApiUrl =
	//	"http://127.0.0.1:8787/api/v3/GetClientConfiguration";
	localConfig_.goLiveApiUrl =
		"https://ingest.twitch.tv/api/v3/GetClientConfiguration";
	localConfig_.streamKey = "";
}

/**
 * Replaces ',' separators in `csvRow` with '\0' string terminators,
 * and appends a pointer to the beginning of every column to `columns`.
 */
void SplitCsvRow(std::vector<const char*>& columns, char* csvRow) {
	while (*csvRow != '\0') {
		columns.push_back(csvRow);
		do {
			csvRow++;
		} while (*csvRow != '\0' && *csvRow != ',');
		if (*csvRow != '\0') {
			*csvRow = '\0';
			csvRow++;
		}
	}
}
struct ParsedCsvRow {
	const char *Application;
	uint64_t ProcessID;
	float TimeInSeconds;
	float msBetweenPresents;
};

class CsvRowParser {
public:
	CsvRowParser();
	bool headerRow(const std::vector<const char *> &columns);
	bool dataRow(const std::vector<const char *> &columns, ParsedCsvRow* dest);
	QString lastError() const;

private:
	// Lots of repetition in the code; given a couple more columns
	// it will be worth using a slightly more complicated generic data structure
	// we can loop over. Something like
	//   struct ColumnFloatTarget { const char* name; int index; float ParsedCsvRow::*destMemberPtr; }
	//   floatTargets.push_back(ColumnFloatTarget("timeInSeconds", n, &ParsedCsvRow::timeInSeconds));
	//   floatTargets.push_back(ColumnFloatTarget("msBetweenPresents", n, &ParsedCsvRow::msBetweenPresents));
	int colApplication_, colProcessID_, colTimeInSeconds_,
		colMsBetweenPresents_;

	QString lastError_;

	void emitError(QString s);
};

CsvRowParser::CsvRowParser()
	: colApplication_(-1),
	  colProcessID_(-1),
	  colTimeInSeconds_(-1),
	  colMsBetweenPresents_(-1)
{
}

QString CsvRowParser::lastError() const
{
	return lastError_;
}

void CsvRowParser::emitError(QString s)
{
	// XXX emit the error!
	// XXX swallow all but the first error?
	lastError_ = s;
	blog(LOG_ERROR, "CsvRowParser::emitError: %s", s.toUtf8());
}

bool CsvRowParser::headerRow(const std::vector<const char*>& columns) {
	for (int i = 0; i < columns.size(); i++) {
		if (0 == strcmp("Application", columns[i])) {
			if (colApplication_ >= 0)
				emitError("Duplicate column Application");
			colApplication_ = i;
		}
		if (0 == strcmp("ProcessID", columns[i])) {
			if (colProcessID_ >= 0)
				emitError("Duplicate column ProcessID");
			colProcessID_ = i;
		}
		if (0 == strcmp("TimeInSeconds", columns[i])) {
			if (colTimeInSeconds_ >= 0)
				emitError("Duplicate column TimeInSeconds");
			colTimeInSeconds_ = i;
		}
		if (0 == strcmp("msBetweenPresents", columns[i])) {
			if (colMsBetweenPresents_ >= 0)
				emitError("Duplicate column msBetweenPresents");
			colMsBetweenPresents_ = i;
		}
	}

	if (colApplication_ == -1)
		emitError("Header missing column Application");
	if (colProcessID_ == -1)
		emitError("Header missing column ProcessID");
	if (colTimeInSeconds_ == -1)
		emitError("Header missing column TimeInSeconds");
	if (colMsBetweenPresents_ == -1)
		emitError("Header missing column msBetweenPresents");

	return lastError_.isEmpty();
}

bool CsvRowParser::dataRow(const std::vector<const char *> &columns,
			   ParsedCsvRow *dest)
{
	if (colApplication_ >= columns.size())
		emitError("Data row missing column Application");
	if (colProcessID_ >= columns.size())
		emitError("Data row missing column ProcessID");
	if (colTimeInSeconds_ >= columns.size())
		emitError("Data row missing column TimeInSeconds");
	if (colMsBetweenPresents_ >= columns.size())
		emitError("Data row missing column msBetweenPresents");

	char *endptr;

	dest->Application = columns[colApplication_];

	dest->ProcessID = strtol(columns[colProcessID_], &endptr, 10);
	if (*endptr != '\0')
		emitError("Data row ProcessID not an int");

	dest->TimeInSeconds = strtod(columns[colTimeInSeconds_], &endptr);
	if (*endptr != '\0')
		emitError("Data row TimeInSeconds not a float");

	dest->msBetweenPresents =
		strtod(columns[colMsBetweenPresents_], &endptr);
	if (*endptr != '\0')
		emitError("Data row msBetweenPresents not a float");

	return lastError_.isEmpty();
}

void afTest() {
	char *x = strdup("hello,world,123546,3234234234,e,f,g,h,99");
	std::vector<const char *> v;
	SplitCsvRow(v, x);
	blog(LOG_INFO, "afTest starting");
	for (auto column : v) {
		blog(LOG_INFO, "afTest: %s", column);
	}

	const char *csvLines[] = {
		"Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.34646610000000,2.79980000000000,33.59020000000000,0,Composed: Flip,68.39570000000001,117.88900000000000,16.61390000000000",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.36793490000000,47.12030000000000,21.46880000000000,0,Composed: Flip,109.20750000000000,146.40840000000000,49.98820000000001",
		"MassEffect3.exe,18580,0x00000194515FA9C0,DXGI,1,0,0,32.42390420000000,53.39470000000000,55.96930000000000,0,Composed: Flip,105.70290000000000,140.43780000000001,49.99870000000000",
		nullptr};

	ParsedCsvRow row;
	CsvRowParser parser;
	for (int i = 0; csvLines[i]; i++) {
		char *s = strdup(csvLines[i]);

		v.clear();
		SplitCsvRow(v, s);

		if (i == 0) {
			bool ok = parser.headerRow(v);
			blog(LOG_INFO,
			     QString("afTest: csv line %1 ok = %2")
				     .arg(i)
				     .arg(ok)
				     .toUtf8());
		} else {
			bool ok = parser.dataRow(v, &row);
			blog(LOG_INFO,
			     QString("afTest: csv line %1 ok = %2, Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
				     .arg(i)
				     .arg(ok)
				     .arg(row.Application)
				     .arg(row.ProcessID)
				     .arg(row.TimeInSeconds)
				     .arg(row.msBetweenPresents)
				     .toUtf8());
		}

		free(s);
	}
}
