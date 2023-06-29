#pragma once

#include <obs.hpp>

class SimulcastOutput {
public:
	/** 
	 * Starts streaming with the given config. Takes ownership of goLiveConfig.
	 */
	void StartStreaming(obs_data_t *goLiveConfig);
	void StopStreaming();
	bool IsStreaming() const;

	/**
	 * Go Live Config used for the current streaming session, if any. Remains
	 * live and unchanged until StopStreaming().
	 */
	obs_data_t *goLiveConfig() const;

private:
	bool streaming_ = false;
	OBSDataAutoRelease goLiveConfig_;
};
