#pragma once

#include "goliveapi-postdata.hpp"

obs_data_t *constructGoLivePost(/* config_t *config, uint32_t fpsNum,
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
