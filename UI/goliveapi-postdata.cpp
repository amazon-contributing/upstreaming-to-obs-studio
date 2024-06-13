#include "goliveapi-postdata.hpp"

#include <nlohmann/json.hpp>

#include "system-info.hpp"

#include "models/multitrack-video.hpp"

GoLiveApi::PostData
constructGoLivePost(QString streamKey,
		    const std::optional<uint64_t> &maximum_aggregate_bitrate,
		    const std::optional<uint32_t> &maximum_video_tracks,
		    bool vod_track_enabled,
		    const std::map<std::string, video_t *> &extra_views)
{
	GoLiveApi::PostData post_data{};
	post_data.service = "IVS";
	post_data.schema_version = "2024-06-04";
	post_data.authentication = streamKey.toStdString();

	system_info(post_data.capabilities);

	auto &client = post_data.client;

	client.name = "obs-studio";
	client.version = obs_get_version_string();

	auto &preferences = post_data.preferences;
	preferences.vod_track_audio = vod_track_enabled;

	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		preferences.width = ovi.output_width;
		preferences.height = ovi.output_height;
		preferences.framerate.numerator = ovi.fps_num;
		preferences.framerate.denominator = ovi.fps_den;

		preferences.canvas_width = ovi.base_width;
		preferences.canvas_height = ovi.base_height;
	}

	if (!extra_views.empty()) {
		post_data.preferences.extra_views.emplace();
		auto &extra_views_capability =
			*post_data.preferences.extra_views;
		extra_views_capability.reserve(extra_views.size());
		for (auto &view : extra_views) {
			video_t *video = view.second;
			if (!video)
				continue;
			const struct video_output_info *voi =
				video_output_get_info(video);
			if (!voi)
				continue;
			extra_views_capability.push_back(GoLiveApi::ExtraView{
				view.first, voi->width, voi->height,
				media_frames_per_second{voi->fps_num,
							voi->fps_den}});
		}
	}

	if (maximum_aggregate_bitrate.has_value())
		preferences.maximum_aggregate_bitrate =
			maximum_aggregate_bitrate.value();
	if (maximum_video_tracks.has_value())
		preferences.maximum_video_tracks = maximum_video_tracks.value();

	return post_data;
}
