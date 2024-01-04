#pragma once

#include <obs.hpp>

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

void RecordingStartHandler(void *arg, calldata_t *data);
void RecordingStopHandler(void *arg, calldata_t *data);

bool SimulcastDeveloperModeEnabled();

struct SimulcastOutput {

public:
	void PrepareStreaming(QWidget *parent, const char *service_name,
			      obs_service_t *service,
			      const std::optional<std::string> &rtmp_url,
			      const QString &stream_key,
			      const char *audio_encoder_id, int audio_bitrate,
			      bool use_ertmp_multitrack,
			      std::optional<uint32_t> maximum_aggregate_bitrate,
			      std::optional<uint32_t> reserved_encoder_sessions,
			      std::optional<std::string> custom_config);
	signal_handler_t *StreamingSignalHandler();
	void StartedStreaming(QWidget *parent, bool success);
	void StopStreaming();
	bool IsStreaming() const;
	std::optional<int> ConnectTimeMs() const;

	bool StartRecording(obs_data_t *go_live_config,
			    bool use_ertmp_multitrack);
	void StopRecording();
	bool IsRecording() const { return recording_; }

	const std::vector<OBSEncoderAutoRelease> &VideoEncoders() const;

	obs_output_t *StreamingOutput() { return output_; }

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

	std::atomic<bool> streaming_ = false;
	std::atomic<bool> recording_ = false;

	std::optional<ImmutableDateTime> stream_attempt_start_time_;

	OBSOutputAutoRelease output_;
	OBSWeakOutputAutoRelease weak_output_;
	std::vector<OBSEncoderAutoRelease> video_encoders_;
	OBSEncoderAutoRelease audio_encoder_;
	OBSServiceAutoRelease simulcast_service_;

	OBSOutputAutoRelease recording_output_;
	OBSWeakOutputAutoRelease weak_recording_output_;

	friend void StreamStartHandler(void *arg, calldata_t *data);
	friend void StreamStopHandler(void *arg, calldata_t *data);
	friend void RecordingStartHandler(void *arg, calldata_t *data);
	friend void RecordingStopHandler(void *arg, calldata_t *data);
};