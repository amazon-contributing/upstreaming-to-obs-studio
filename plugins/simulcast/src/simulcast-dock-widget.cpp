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

#include "berryessa-submitter.hpp"
#include "goliveapi-network.hpp"
#include "goliveapi-postdata.hpp"
#include "presentmon-csv-parser.hpp"

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


void afTest()
{
	testCsvParser();

	// frame stuff
	// XXX where to put this?
	QProcess* process = new QProcess();

	// Log a bunch of QProcess signals
	QObject::connect(process, &QProcess::started, []() {
		blog(LOG_INFO, "QProcess::started received");
	});
	QObject::connect(process, &QProcess::errorOccurred, [](QProcess::ProcessError error) {
		blog(LOG_INFO, QString("QProcess::errorOccurred(error=%1) received").arg(error).toUtf8());
	});
	QObject::connect(
		process, &QProcess::stateChanged,
		[](QProcess::ProcessState newState) {
			blog(LOG_INFO,
			     QString("QProcess::stateChanged(newState=%1) received")
				     .arg(newState)
				     .toUtf8());
	});
	QObject::connect(
		process, &QProcess::finished,
		[](int exitCode, QProcess::ExitStatus exitStatus) {
			blog(LOG_INFO,
			     QString("QProcess::finished(exitCode=%1, exitStatus=%2) received")
				     .arg(exitCode)
				     .arg(exitStatus)
				     .toUtf8());
		});

	QObject::connect(process, &QProcess::readyReadStandardError, [process]() {
			QByteArray data;
			while ((data = process->readAllStandardError()).size()) {
				blog(LOG_INFO, "StdErr: %s", data.constData());
			}
		});

	// Process the CSV as it appears on stdout
	// This will be better as a class member than a closure, because we have
	// state, and autoformat at 80 columns with size-8 tabs is really yucking
	// things up!
	struct state_t {
		state_t() : alreadyErrored(false), lineNumber(0) {}

		bool alreadyErrored;
		uint64_t lineNumber;
		std::vector<const char *> v;
		ParsedCsvRow row;
		CsvRowParser parser;
	};
	state_t *state = new state_t;
	QObject::
		connect(process, &QProcess::readyReadStandardOutput,
			 [state, process]() {
				char buf[1024];
			if (state->alreadyErrored) {
					qint64 n = process->readLine(
						buf, sizeof(buf));
					blog(LOG_INFO, "POST-ERROR line %d: %s",
					     state->lineNumber, buf);
				state->lineNumber++;
				}
				while (!state->alreadyErrored && process->canReadLine()) {
					qint64 n = process->readLine(
							buf, sizeof(buf));

					if (n < 1 || n >= sizeof(buf) - 1) {
						// XXX emit error
						state->alreadyErrored = true;
					} else {
						if (buf[n - 1] == '\n') {
							//blog(LOG_INFO,
							//"REPLACING NEWLINE WITH NEW NULL");
							buf[n - 1] = '\0';
						}

						if (state->lineNumber < 10) {
							blog(LOG_INFO,
							     "Got line %d: %s",
							     state->lineNumber,
							     buf);
						}

						state->v.clear();
						SplitCsvRow(state->v, buf);

						if (state->lineNumber == 0) {
							state->alreadyErrored =
								!state->parser.headerRow(
									state->v);
						} else {
							state->alreadyErrored =
								!state->parser.dataRow(
									state->v, &state->row);

							if (!state->alreadyErrored &&
							    state->lineNumber < 10) {
							blog(LOG_INFO,
							     QString("afTest: csv line %1 Application=%3, ProcessID=%4, TimeInSeconds=%5, msBetweenPresents=%6")
								     .arg(state->lineNumber)
								     .arg(state->row.Application)
								     .arg(state->row.ProcessID)
								     .arg(state->row.TimeInSeconds)
								     .arg(state->row.msBetweenPresents)
								     .toUtf8());
							}
						}
						state->lineNumber++;
					}
				}
			 });


	// Start the process
	QStringList args({"-output_stdout", "-stop_existing_session"});
	process->start(
		"c:\\obsdev\\PresentMon\\build\\Release\\PresentMon-dev-x64.exe",
		args, QIODeviceBase::ReadWrite);
}
