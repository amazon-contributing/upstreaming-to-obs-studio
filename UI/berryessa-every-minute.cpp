
#include "berryessa-submitter.hpp"
#include "berryessa-every-minute.hpp"
#include "presentmon-csv-capture.hpp"

#include <util/dstr.hpp>

#include <QTimer>
#include <QRandomGenerator>
#include <QDateTime>
#include <QThreadPool>
#include <QPointer>

#include <chrono>

static OBSFrameCounters InitFrameCounters();

BerryessaEveryMinute::BerryessaEveryMinute(
	QObject * /*parent*/, BerryessaSubmitter *berryessa,
	const std::vector<OBSEncoder> &encoders)
	: QObject(),
	  berryessa_(berryessa),
	  presentmon_(this),
	  timer_(this),
	  startTime_(QDateTime::currentDateTimeUtc()),
	  shared_counters_(std::make_shared<UsageInfoCounters>())
{

#ifdef WIN32
	shared_counters_->wmi_queries_ = WMIQueries::Create();
#endif
	shared_counters_->frame_counters_ = InitFrameCounters();

	auto &encoder_counters = shared_counters_->encoder_counters_;
	encoder_counters.reserve(encoders.size());
	for (const auto &encoder : encoders) {
		auto video = obs_encoder_video(encoder);
		encoder_counters.push_back(OBSEncoderFrameCounters{
			OBSWeakEncoderAutoRelease(
				obs_encoder_get_weak_encoder(encoder)),
			video_output_get_total_frames(video),
			video_output_get_skipped_frames(video),
		});
	}

	shared_counters_->obs_cpu_usage_info_.reset(os_cpu_usage_info_start());

	connect(&timer_, &QTimer::timeout, this, &BerryessaEveryMinute::fire);

	timer_.setSingleShot(true);
	quint64 msecs = QRandomGenerator::global()->generate64() % 60000;
	blog(LOG_INFO, "BerryessaEveryMinute - first invocation in %d ms",
	     (int)msecs);
	timer_.start(msecs);
}

BerryessaEveryMinute::~BerryessaEveryMinute() {}

static OBSFrameCounters InitFrameCounters()
{
	OBSFrameCounters frame_counters{};

	auto video = obs_get_video();
	frame_counters.skipped = video_output_get_skipped_frames(video);
	frame_counters.output = video_output_get_total_frames(video);

	frame_counters.rendered = obs_get_total_frames();
	frame_counters.lagged = obs_get_lagged_frames();

	return frame_counters;
}

static void AddOBSStats(os_cpu_usage_info *info,
			OBSFrameCounters &frame_counters,
			std::vector<OBSEncoderFrameCounters> &encoder_counters,
			obs_data_t *event)
{
	obs_data_set_double(event, "obs_cpu_usage",
			    os_cpu_usage_info_query(info));
	obs_data_set_double(event, "current_obs_fps", obs_get_active_fps());
	obs_data_set_int(event, "obs_memory_usage",
			 os_get_proc_resident_size());

	obs_data_set_int(event, "total_memory", os_get_sys_total_size());
	obs_data_set_int(event, "free_memory", os_get_sys_free_size());

	// FIXME: these return 0 currently for plugin streams
	auto video = obs_get_video();
	auto skipped_frames = video_output_get_skipped_frames(video);
	auto output_frames = video_output_get_total_frames(video);

	auto rendered_frames = obs_get_total_frames();
	auto lagged_frames = obs_get_lagged_frames();

	auto maybe_update = [](uint32_t &holder, uint32_t val) {
		if (holder > val)
			holder = val;
	};
	maybe_update(frame_counters.output, output_frames);
	maybe_update(frame_counters.skipped, skipped_frames);
	maybe_update(frame_counters.rendered, rendered_frames);
	maybe_update(frame_counters.lagged, lagged_frames);

	obs_data_set_int(event, "skipped_frames",
			 skipped_frames - frame_counters.skipped);
	obs_data_set_int(event, "output_frames",
			 output_frames - frame_counters.output);

	obs_data_set_int(event, "rendered_frames",
			 rendered_frames - frame_counters.rendered);
	obs_data_set_int(event, "lagged_frames",
			 lagged_frames - frame_counters.lagged);

	frame_counters.skipped = skipped_frames;
	frame_counters.output = output_frames;
	frame_counters.rendered = rendered_frames;
	frame_counters.lagged = lagged_frames;

	DStr buffer;
	for (size_t i = 0; i < encoder_counters.size(); i++) {
		auto &counter = encoder_counters[i];
		OBSEncoderAutoRelease encoder =
			obs_weak_encoder_get_encoder(counter.weak_output);
		if (!encoder)
			continue;
		auto video = obs_encoder_video(encoder);
		auto output_frames = video_output_get_total_frames(video);
		auto skipped_frames = video_output_get_skipped_frames(video);

		dstr_printf(buffer, "encoder%zu_total_frames", i);
		obs_data_set_int(event, buffer->array,
				 output_frames - counter.output);
		dstr_printf(buffer, "encoder%zu_skipped_frames", i);
		obs_data_set_int(event, buffer->array,
				 skipped_frames - counter.skipped);

		counter.output = output_frames;
		counter.skipped = skipped_frames;
	}
}

void BerryessaEveryMinute::fire()
{
	if (timer_.isSingleShot()) {
		timer_.setSingleShot(false);
		// precise timers are supposed to produce timer stability
		// with low jitter and stability over time
		timer_.setTimerType(Qt::PreciseTimer);
		timer_.start(std::chrono::minutes(1));
	}

	blog(LOG_INFO, "BerryessaEveryMinute::fire called");

	OBSDataAutoRelease event = obs_data_create();

	presentmon_.summarizeAndReset(event);

	QThreadPool::globalInstance()->start(
		[shared_counters = shared_counters_,
		 berryessa = QPointer<BerryessaSubmitter>(berryessa_),
		 event = OBSData{event}] {
			AddOBSStats(shared_counters->obs_cpu_usage_info_.get(),
				    shared_counters->frame_counters_,
				    shared_counters->encoder_counters_, event);

#ifdef _WIN32
			if (shared_counters->wmi_queries_.has_value())
				shared_counters->wmi_queries_->SummarizeData(
					event);
#endif
			auto buffer = QDateTime::currentDateTimeUtc()
					      .toString(Qt::ISODateWithMs)
					      .toUtf8();
			obs_data_set_string(event, "time_utc",
					    buffer.constData());

			QMetaObject::invokeMethod(berryessa, [event,
							      berryessa] {
				berryessa->submit("ivs_obs_stream_minute",
						  event);
			});
		});
}
