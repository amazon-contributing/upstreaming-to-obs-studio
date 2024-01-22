#pragma once

#include <obs.hpp>
#include <optional>
#include <map>
#include <QString>

struct ImmutableDateTime;
struct MultitrackVideoViewInfo;

OBSDataAutoRelease
constructGoLivePost(const ImmutableDateTime &attempt_start_time,
		    QString streamKey,
		    const std::optional<uint64_t> &maximum_aggregate_bitrate,
		    const std::optional<uint32_t> &maximum_video_tracks,
		    std::map<std::string, video_t *> &extra_views);
