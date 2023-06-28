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

#include <obs.hpp>
#include <obs-module.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>



#define ConfigSection "simulcast"

#include "berryessa-submitter.hpp"
#include "goliveapi-network.hpp"
#include "goliveapi-postdata.hpp"
#include "presentmon-csv-capture.hpp"

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

					StartPresentMon();

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


