#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <QObject>

#define NOMINMAX

#include "berryessa-submitter.hpp"
#include "berryessa-every-minute.hpp"

class QString;

void StreamStopHandler(void *arg, calldata_t *data);
void StreamDeactivateHandler(void *arg, calldata_t *data);

void RecordingStartHandler(void *arg, calldata_t *data);
void RecordingStopHandler(void *arg, calldata_t *data);
void RecordingDeactivateHandler(void *arg, calldata_t *data);

bool MultitrackVideoDeveloperModeEnabled();

struct FutureWaitGuard {
	FutureWaitGuard(std::shared_future<void> future_) : future(future_) {}
	FutureWaitGuard(const FutureWaitGuard &) = delete;
	~FutureWaitGuard()
	{
		if (!future.valid())
			return;
		future.wait();
	}

	std::shared_future<void> future;
};

struct MultitrackVideoViewInfo {
	inline MultitrackVideoViewInfo(const char *name_,
				       multitrack_video_start_cb start_video_,
				       multitrack_video_stop_cb stop_video_,
				       void *param_)
		: start_video(start_video_),
		  stop_video(stop_video_),
		  param(param_),
		  name(name_)
	{
	}
	std::string name;
	multitrack_video_start_cb start_video = nullptr;
	multitrack_video_stop_cb stop_video = nullptr;
	void *param = nullptr;
};

struct MultitrackVideoOutput {
public:
	void PrepareStreaming(QWidget *parent, const char *service_name,
			      obs_service_t *service,
			      const std::optional<std::string> &rtmp_url,
			      const QString &stream_key,
			      const char *audio_encoder_id,
			      std::optional<uint32_t> maximum_aggregate_bitrate,
			      std::optional<uint32_t> maximum_video_tracks,
			      std::optional<std::string> custom_config,
			      obs_data_t *dump_stream_to_file_config,
			      size_t main_audio_mixer,
			      std::optional<size_t> vod_track_mixer,
			      std::optional<bool> use_rtmps);
	signal_handler_t *StreamingSignalHandler();
	void StartedStreaming(QWidget *parent);
	void StopStreaming();
	bool HandleIncompatibleSettings(QWidget *parent, config_t *config,
					obs_service_t *service, bool &useDelay);

	OBSOutputAutoRelease StreamingOutput()
	{
		const std::lock_guard current_lock{current_mutex};
		return current ? obs_output_get_ref(current->output_) : nullptr;
	}

	void StopExtraViews();

private:
	std::unique_ptr<BerryessaSubmitter> berryessa_;
	std::shared_ptr<std::optional<BerryessaEveryMinute>>
		berryessa_every_minute_ =
			std::make_shared<std::optional<BerryessaEveryMinute>>(
				std::nullopt);
	std::optional<FutureWaitGuard> berryessa_every_minute_initializer_;

	struct OBSOutputObjects {
		OBSOutputAutoRelease output_;
		std::shared_ptr<obs_encoder_group_t> video_encoder_group_;
		std::vector<OBSEncoderAutoRelease> video_encoders_;
		std::vector<OBSEncoderAutoRelease> audio_encoders_;
		OBSServiceAutoRelease multitrack_video_service_;
		OBSSignal start_signal, stop_signal, deactivate_signal;
	};
	std::map<std::string, video_t *> extra_views_;

	std::optional<OBSOutputObjects> take_current();
	std::optional<OBSOutputObjects> take_current_stream_dump();

	static void
	ReleaseOnMainThread(std::optional<OBSOutputObjects> objects);

	std::mutex current_mutex;
	std::optional<OBSOutputObjects> current;

	std::mutex current_stream_dump_mutex;
	std::optional<OBSOutputObjects> current_stream_dump;

	friend void StreamStopHandler(void *arg, calldata_t *data);
	friend void StreamDeactivateHandler(void *arg, calldata_t *data);
	friend void RecordingStartHandler(void *arg, calldata_t *data);
	friend void RecordingStopHandler(void *arg, calldata_t *data);
	friend void RecordingDeactivateHandler(void *arg, calldata_t *data);
};
