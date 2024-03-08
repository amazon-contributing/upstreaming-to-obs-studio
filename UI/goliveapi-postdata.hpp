#pragma once

#include <obs.hpp>
#include <optional>
#include <QString>

struct ImmutableDateTime;

OBSDataAutoRelease
constructGoLivePost(const ImmutableDateTime &attempt_start_time,
		    QString streamKey,
		    const std::optional<uint64_t> &maximum_aggregate_bitrate,
		    const std::optional<uint32_t> &maximum_video_tracks);
