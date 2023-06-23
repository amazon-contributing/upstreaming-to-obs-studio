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
#include "remote-text.hpp"


#define ConfigSection "simulcast"

#include "goliveapi-request.h"

static obs_data_t *constructGoLivePost(/* config_t *config, uint32_t fpsNum,
				       uint32_t fpsDen*/)
{
	obs_data_t *postData = obs_data_create();
	OBSDataAutoRelease capabilitiesData = obs_data_create();
	obs_data_set_string(postData, "service", "IVS");
	obs_data_set_string(postData, "schema_version", "2023-05-10");
	obs_data_set_obj(postData, "capabilities", capabilitiesData);

	obs_data_set_bool(capabilitiesData, "plugin", true);

#if 0
	OBSDataArray adapters = obs_device_adapter_data();
	obs_data_set_array(capabilitiesData, "gpu", adapters);

	OBSData systemData =
		os_get_system_info(); // XXX autorelease vs set_obj vs apply?
	obs_data_apply(capabilitiesData, systemData);

	OBSData clientData = obs_data_create();
	obs_data_set_obj(capabilitiesData, "client", clientData);
	obs_data_set_string(clientData, "name", "obs-studio");
	obs_data_set_string(clientData, "version", obs_get_version_string());
	obs_data_set_int(clientData, "width",
			 config_get_uint(config, "Video", "OutputCX"));
	obs_data_set_int(clientData, "height",
			 config_get_uint(config, "Video", "OutputCY"));
	obs_data_set_int(clientData, "fps_numerator", fpsNum);
	obs_data_set_int(clientData, "fps_denominator", fpsDen);

	// XXX hardcoding the present-day AdvancedOutput behavior here..
	// XXX include rescaled output size?
	OBSData encodingData = AdvancedOutputStreamEncoderSettings();
	obs_data_set_string(encodingData, "type",
			    config_get_string(config, "AdvOut", "Encoder"));
	unsigned int cx, cy;
	AdvancedOutputGetRescaleRes(config, &cx, &cy);
	if (cx && cy) {
		obs_data_set_int(encodingData, "width", cx);
		obs_data_set_int(encodingData, "height", cy);
	}

	OBSDataArray encodingDataArray = obs_data_array_create();
	obs_data_array_push_back(encodingDataArray, encodingData);
	obs_data_set_array(postData, "client_encoder_configurations",
			   encodingDataArray);
#endif

	//XXX todo network.speed_limit

	return postData;
}

static obs_data_t* DownloadGoLiveConfig(QString url)
{
	OBSDataAutoRelease postData = constructGoLivePost();
#if 0
	uint32_t fpsNum, fpsDen;
	GetConfigFPS(fpsNum, fpsDen);
	OBSDataAutoRelease postData =
		constructGoLivePost(this->Config(), fpsNum, fpsDen);
#endif
	blog(LOG_INFO, "Go live POST data: %s", obs_data_get_json(postData));

	// andrew download code start
	QString encodeConfigError;
	OBSData encodeConfigObsData;

	std::string encodeConfigText;
	std::string libraryError;

	std::vector<std::string> headers;
	headers.push_back("Content-Type: application/json");
	bool encodeConfigDownloadedOk = GetRemoteFile(
		url.toLocal8Bit(), encodeConfigText, libraryError, // out params
		nullptr, nullptr, // out params (response code and content type)
		"POST", obs_data_get_json(postData), headers,
		nullptr, // signature
		3);      // timeout in seconds

	if (!encodeConfigDownloadedOk) {
		encodeConfigError =
			QString() + "Could not fetch config from " +
			url + "\n\nHTTP error: " +
			QString::fromStdString(libraryError) +
			"\n\nDo you want to stream anyway? You'll only stream a single quality option.";
	} else {
#if 0
		// XXX: entirely different json parser just because it gives us errors
		// is a bit silly
		Json encodeConfigJson =
			Json::parse(encodeConfigText, libraryError);
		if (!encodeConfigJson.is_object()) {
			encodeConfigError =
				QString() + "JSON parse error: " +
				QString::fromStdString(libraryError);
		}
#endif
		encodeConfigObsData =
			obs_data_create_from_json(encodeConfigText.c_str());
	}

	if (!encodeConfigError.isEmpty()) {
		int carryOn = QMessageBox::warning(
			nullptr /*this*/, "Multi-encode Staff Beta Error",
			encodeConfigError, QMessageBox::Yes, QMessageBox::No);

		if (carryOn != QMessageBox::Yes)
			return nullptr;//false;

		encodeConfigObsData = nullptr;
	}

	blog(LOG_INFO, "Go Live Config data: %s", encodeConfigText.c_str());

#if 0
	if (encodeConfigObsData) {
		this->goLiveConfigData = encodeConfigObsData;
		this->ResetOutputs();
	}

	return true;
#endif
	return encodeConfigObsData;
}


SimulcastDockWidget::SimulcastDockWidget(QWidget *parent)
{
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
				streamingButton->setText(
					obs_module_text("Btn.StartStreaming"));
			} else {
				obs_data_t *goLiveConfig = DownloadGoLiveConfig(
					localConfig_.goLiveApiUrl);
				if (goLiveConfig) {
					this->Output().StartStreaming(
						goLiveConfig);
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
