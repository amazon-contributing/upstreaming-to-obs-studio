#include "simulcast-output.h"

void SimulcastOutput::StartStreaming(obs_data_t* goLiveConfig)
{
	streaming_ = true;
	goLiveConfig_ = goLiveConfig;
}

void SimulcastOutput::StopStreaming()
{
	streaming_ = false;
	goLiveConfig_ = nullptr;
}

bool SimulcastOutput::IsStreaming() const
{
	return streaming_;
}

obs_data_t* SimulcastOutput::goLiveConfig() const
{
	return goLiveConfig_;
}
