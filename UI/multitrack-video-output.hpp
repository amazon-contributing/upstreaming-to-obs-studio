#pragma once

#include <obs.hpp>
#include <util/config-file.h>

#include <atomic>
#include <optional>
#include <vector>

#include <qobject.h>
#include <QFuture>
#include <QFutureSynchronizer>

#define NOMINMAX

#include "immutable-date-time.hpp"

#include "berryessa-submitter.hpp"
#include "berryessa-every-minute.hpp"

class QString;

void StreamStartHandler(void *arg, calldata_t *data);
void StreamStopHandler(void *arg, calldata_t *data);
void StreamDeactivateHandler(void *arg, calldata_t *data);

void RecordingStartHandler(void *arg, calldata_t *data);
void RecordingStopHandler(void *arg, calldata_t *data);
void RecordingDeactivateHandler(void *arg, calldata_t *data);

bool MultitrackVideoDeveloperModeEnabled();

struct MultitrackVideoOutput {

public:
	void PrepareStreaming(QWidget *parent, const char *service_name,
			      obs_service_t *service,
			      const std::optional<std::string> &rtmp_url,
			      const QString &stream_key,
			      const char *audio_encoder_id, int audio_bitrate,
			      std::optional<uint32_t> maximum_aggregate_bitrate,
			      std::optional<uint32_t> maximum_video_tracks,
			      std::optional<std::string> custom_config,
			      obs_data_t *dump_stream_to_file_config);
	signal_handler_t *StreamingSignalHandler();
	void StartedStreaming(QWidget *parent, bool success);
	void StopStreaming();
	std::optional<int> ConnectTimeMs() const;
	bool HandleIncompatibleSettings(QWidget *parent, config_t *config,
					obs_service_t *service, bool &useDelay,
					bool &enableNewSocketLoop,
					bool &enableDynBitrate);

	obs_output_t *StreamingOutput()
	{
		return current ? &*current->output_ : nullptr;
	}

private:
	const ImmutableDateTime &GenerateStreamAttemptStartTime();

	std::unique_ptr<BerryessaSubmitter> berryessa_;
	std::shared_ptr<std::optional<BerryessaEveryMinute>>
		berryessa_every_minute_ =
			std::make_shared<std::optional<BerryessaEveryMinute>>(
				std::nullopt);
	QFutureSynchronizer<void> berryessa_every_minute_initializer_;

	std::function<void(bool success, std::optional<int> connect_time_ms)>
		send_start_event;

	std::optional<ImmutableDateTime> stream_attempt_start_time_;

	struct OBSOutputObjects {
		OBSOutputAutoRelease output_;
		std::vector<OBSEncoderAutoRelease> video_encoders_;
		std::vector<OBSEncoderAutoRelease> audio_encoders_;
		OBSServiceAutoRelease multitrack_video_service_;
		OBSSignal start_signal, stop_signal, deactivate_signal;
	};

	std::optional<OBSOutputObjects> current;
	std::optional<OBSOutputObjects> current_stream_dump;

	friend void StreamStartHandler(void *arg, calldata_t *data);
	friend void StreamStopHandler(void *arg, calldata_t *data);
	friend void StreamDeactivateHandler(void *arg, calldata_t *data);
	friend void RecordingStartHandler(void *arg, calldata_t *data);
	friend void RecordingStopHandler(void *arg, calldata_t *data);
	friend void RecordingDeactivateHandler(void *arg, calldata_t *data);
};
