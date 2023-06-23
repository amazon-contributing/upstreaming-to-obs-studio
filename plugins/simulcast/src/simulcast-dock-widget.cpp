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

#include <obs.hpp>
#include <obs-module.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>


#define ConfigSection "simulcast"

#include "goliveapi-network.hpp"
#include "goliveapi-postdata.hpp"
#include "berryessa-submitter.hpp"

SimulcastDockWidget::SimulcastDockWidget(QWidget *parent)
{
	berryessa_ = new BerryessaSubmitter(this, "http://127.0.0.1:8787/");

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

					obs_data_t *event = obs_data_create();
					obs_data_set_int(event, "encoder_count",
							 1); // XXX hardcoded value
					this->berryessa_->submit(
						"ivs_obs_stream_start", event);

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
