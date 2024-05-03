/*
 * Copyright (c) 2024 Ruwen Hahn <haruwenz@twitch.tv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <string>
#include <optional>

#include <nlohmann/json.hpp>

/* From whatsnew.hpp */
#ifndef NLOHMANN_DEFINE_TYPE_INTRUSIVE
#define NLOHMANN_DEFINE_TYPE_INTRUSIVE(Type, ...)                             \
	friend void to_json(nlohmann::json &nlohmann_json_j,                  \
			    const Type &nlohmann_json_t)                      \
	{                                                                     \
		NLOHMANN_JSON_EXPAND(                                         \
			NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__))   \
	}                                                                     \
	friend void from_json(const nlohmann::json &nlohmann_json_j,          \
			      Type &nlohmann_json_t)                          \
	{                                                                     \
		NLOHMANN_JSON_EXPAND(                                         \
			NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, __VA_ARGS__)) \
	}
#endif

/*
 * Support for (de-)serialising std::optional
 * From https://github.com/nlohmann/json/issues/1749#issuecomment-1731266676
 * whatsnew.hpp's version doesn't seem to work here
 */
template<typename T> struct nlohmann::adl_serializer<std::optional<T>> {
	static void from_json(const json &j, std::optional<T> &opt)
	{
		if (j.is_null()) {
			opt = std::nullopt;
		} else {
			opt = j.get<T>();
		}
	}
	static void to_json(json &json, std::optional<T> t)
	{
		if (t) {
			json = *t;
		} else {
			json = nullptr;
		}
	}
};

namespace GoLiveApi {
using std::string;
using std::optional;

struct Client {
	string name = "obs-studio";
	string version;
	bool vod_track_audio;
	uint32_t width;
	uint32_t height;
	uint32_t fps_numerator;
	uint32_t fps_denominator;
	uint32_t canvas_width;
	uint32_t canvas_height;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Client, name, version, vod_track_audio,
				       width, height, fps_numerator,
				       fps_denominator, canvas_width,
				       canvas_height)
};

struct Cpu {
	int32_t physical_cores;
	int32_t logical_cores;
	optional<uint32_t> speed;
	optional<string> name;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Cpu, physical_cores, logical_cores,
				       speed, name)
};

struct Memory {
	uint64_t total;
	uint64_t free;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Memory, total, free)
};

struct Gpu {
	string model;
	uint32_t vendor_id;
	uint32_t device_id;
	uint64_t dedicated_video_memory;
	uint64_t shared_system_memory;
	string luid;
	optional<string> driver_version;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Gpu, model, vendor_id, device_id,
				       dedicated_video_memory,
				       shared_system_memory, luid,
				       driver_version)
};

struct GamingFeatures {
	optional<bool> game_bar_enabled;
	optional<bool> game_dvr_allowed;
	optional<bool> game_dvr_enabled;
	optional<bool> game_dvr_bg_recording;
	optional<bool> game_mode_enabled;
	optional<bool> hags_enabled;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(GamingFeatures, game_bar_enabled,
				       game_dvr_allowed, game_dvr_enabled,
				       game_dvr_bg_recording, game_mode_enabled,
				       hags_enabled)
};

struct System {
	string version;
	string name;
	int build;
	string release;
	int revision;
	int bits;
	bool arm;
	bool armEmulation;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(System, version, name, build, release,
				       revision, bits, arm, armEmulation)
};

struct ExtraView {
	string name;
	uint32_t canvas_width;
	uint32_t canvas_height;
	media_frames_per_second fps;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ExtraView, name, canvas_width,
				       canvas_height, fps)
};

struct Capabilities {
	Client client;
	Cpu cpu;
	Memory memory;
	optional<GamingFeatures> gaming_features;
	System system;
	optional<std::vector<Gpu>> gpu;
	optional<std::vector<ExtraView>> extra_views;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Capabilities, client, cpu, memory,
				       gaming_features, system, gpu,
				       extra_views)
};

struct Preferences {
	optional<uint64_t> maximum_aggregate_bitrate;
	optional<uint32_t> maximum_video_tracks;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Preferences, maximum_aggregate_bitrate,
				       maximum_video_tracks)
};

struct PostData {
	string service = "IVS";
	string schema_version = "2023-05-10";
	string stream_attempt_start_time;
	string authentication;

	Capabilities capabilities;
	Preferences preferences;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(PostData, service, schema_version,
				       stream_attempt_start_time,
				       authentication, capabilities,
				       preferences)
};

} // namespace GoLiveApi
