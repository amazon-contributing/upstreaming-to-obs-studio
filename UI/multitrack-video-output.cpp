#include "multitrack-video-output.hpp"

#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <util/util.hpp>
#include <obs-frontend-api.h>
#include <obs-app.hpp>
#include <obs.hpp>
#include <remote-text.hpp>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QScopeGuard>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <nlohmann/json.hpp>

#include "system-info.hpp"
#include "goliveapi-postdata.hpp"
#include "goliveapi-network.hpp"
#include "ivs-events.hpp"
#include "multitrack-video-error.hpp"
#include "qt-helpers.hpp"

Qt::ConnectionType BlockingConnectionTypeFor(QObject *object)
{
	return object->thread() == QThread::currentThread()
		       ? Qt::DirectConnection
		       : Qt::BlockingQueuedConnection;
}

bool MultitrackVideoDeveloperModeEnabled()
{
	static bool developer_mode = [] {
		auto args = qApp->arguments();
		for (const auto &arg : args) {
			if (arg == "--enable-multitrack-video-dev") {
				return true;
			}
		}
		return false;
	}();
	return developer_mode;
}

static const QString &device_id()
{
	static const QString device_id_ = []() -> QString {
		auto config = App()->GlobalConfig();
		if (!config_has_user_value(config, "General", "DeviceID")) {

			auto new_device_id = QUuid::createUuid().toString(
				QUuid::WithoutBraces);
			config_set_string(config, "General", "DeviceID",
					  new_device_id.toUtf8().constData());
		}
		return config_get_string(config, "General", "DeviceID");
	}();
	return device_id_;
}

static const QString &obs_session_id()
{
	static const QString session_id_ =
		QUuid::createUuid().toString(QUuid::WithoutBraces);
	return session_id_;
}

static void submit_event(BerryessaSubmitter *berryessa, const char *event_name,
			 obs_data_t *data)
{
	if (!berryessa) {
		blog(LOG_WARNING,
		     "MultitrackVideoOutput: not submitting event %s",
		     event_name);
		return;
	}

	berryessa->submit(event_name, data);
}

static void add_always_bool(BerryessaSubmitter *berryessa, const char *name,
			    bool data)
{
	if (!berryessa)
		return;

	berryessa->setAlwaysBool(name, data);
}

static void add_always_string(BerryessaSubmitter *berryessa, const char *name,
			      const char *data)
{
	if (!berryessa)
		return;

	berryessa->setAlwaysString(name, data);
}

static OBSServiceAutoRelease
create_service(const QString &device_id, const QString &obs_session_id,
	       obs_data_t *go_live_config,
	       const std::optional<std::string> &rtmp_url,
	       const QString &in_stream_key)
{
	const char *url = nullptr;
	QString stream_key = in_stream_key;
	if (rtmp_url.has_value()) {
		url = rtmp_url->c_str();

		// Despite being set by user, it was set to a ""
		if (rtmp_url->empty()) {
			throw MultitrackVideoError::warning(QTStr(
				"FailedToStartStream.NoCustomRTMPURLInSettings"));
		}
		blog(LOG_INFO, "Using custom rtmp URL: '%s'", url);
	} else {
		OBSDataArrayAutoRelease ingest_endpoints =
			obs_data_get_array(go_live_config, "ingest_endpoints");
		for (size_t i = 0; i < obs_data_array_count(ingest_endpoints);
		     i++) {
			OBSDataAutoRelease item =
				obs_data_array_item(ingest_endpoints, i);
			if (qstrnicmp("RTMP",
				      obs_data_get_string(item, "protocol"), 4))
				continue;

			url = obs_data_get_string(item, "url_template");
			blog(LOG_INFO, "Using URL template: '%s'", url);
			const char *sk =
				obs_data_get_string(item, "authentication");
			if (sk && *sk) {
				blog(LOG_INFO,
				     "Using stream key supplied by autoconfig");
				stream_key = sk;
			}
			break;
		}

		if (!url) {
			blog(LOG_ERROR, "No RTMP URL in go live config");
			throw MultitrackVideoError::warning(
				QTStr("FailedToStartStream.NoRTMPURLInConfig"));
		}
	}

	DStr str;
	dstr_cat(str, url);

	{
		// dstr_find does not protect against null, and dstr_cat will
		// not inialize str if cat'ing with a null url
		if (!dstr_is_empty(str)) {
			auto found = dstr_find(str, "/{stream_key}");
			if (found)
				dstr_remove(str, found - str->array,
					    str->len - (found - str->array));
		}
	}

	QUrl parsed_url{url};
	QUrlQuery parsed_query{parsed_url};

	parsed_query.addQueryItem("deviceIdentifier", device_id);
	parsed_query.addQueryItem("obsSessionId", obs_session_id);

	OBSDataAutoRelease go_live_meta =
		obs_data_get_obj(go_live_config, "meta");
	if (go_live_meta) {
		const char *config_id =
			obs_data_get_string(go_live_meta, "config_id");
		if (config_id && *config_id) {
			parsed_query.addQueryItem("obsConfigId", config_id);
		}
	}

	auto key_with_param = stream_key + "?" + parsed_query.toString();

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", str->array);
	obs_data_set_string(settings, "key",
			    key_with_param.toUtf8().constData());

	auto service = obs_service_create(
		"rtmp_custom", "multitrack video service", settings, nullptr);

	if (!service) {
		blog(LOG_WARNING, "Failed to create multitrack video service");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateMultitrackVideoService"));
	}

	return service;
}

static void ensure_directory_exists(std::string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == std::string::npos)
		return;

	std::string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

std::string GetOutputFilename(const std::string &path, const char *format)
{
	std::string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\')
		strPath += "/";

	strPath += BPtr<char>{
		os_generate_formatted_filename("flv", false, format)};
	ensure_directory_exists(strPath);

	return strPath;
}

static OBSOutputAutoRelease create_output()
{
	OBSOutputAutoRelease output = obs_output_create(
		"rtmp_output", "rtmp multitrack video", nullptr, nullptr);

	if (!output) {
		blog(LOG_ERROR,
		     "failed to create multitrack video rtmp output");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateMultitrackVideoOutput"));
	}

	return output;
}

static OBSOutputAutoRelease create_recording_output(obs_data_t *settings)
{
	OBSOutputAutoRelease output = obs_output_create(
		"flv_output", "flv multitrack video", settings, nullptr);

	if (!output)
		blog(LOG_ERROR, "failed to create multitrack video flv output");

	return output;
}

static void data_item_release(obs_data_item_t *item)
{
	obs_data_item_release(&item);
}

using OBSDataItemAutoRelease =
	OBSRefAutoRelease<obs_data_item_t *, data_item_release>;

static obs_scale_type load_gpu_scale_type(obs_data_t *encoder_config)
{
	const auto default_scale_type = OBS_SCALE_BICUBIC;

	OBSDataItemAutoRelease item =
		obs_data_item_byname(encoder_config, "gpu_scale_type");
	if (!item)
		return default_scale_type;

	switch (obs_data_item_gettype(item)) {
	case OBS_DATA_NUMBER: {
		auto val = obs_data_item_get_int(item);
		if (val < OBS_SCALE_POINT || val > OBS_SCALE_AREA) {
			blog(LOG_WARNING,
			     "load_gpu_scale_type: scale type out of range %lld (must be 1 <= value <= 5)",
			     val);
			break;
		}
		return static_cast<obs_scale_type>(val);
	}

	case OBS_DATA_STRING: {
		auto val = obs_data_item_get_string(item);
		if (strncmp(val, "OBS_SCALE_POINT", 16) == 0)
			return OBS_SCALE_POINT;
		if (strncmp(val, "OBS_SCALE_BICUBIC", 18) == 0)
			return OBS_SCALE_BICUBIC;
		if (strncmp(val, "OBS_SCALE_BILINEAR", 19) == 0)
			return OBS_SCALE_BILINEAR;
		if (strncmp(val, "OBS_SCALE_LANCZOS", 18) == 0)
			return OBS_SCALE_LANCZOS;
		if (strncmp(val, "OBS_SCALE_AREA", 15) == 0)
			return OBS_SCALE_AREA;
		blog(LOG_WARNING,
		     "load_gpu_scale_type: unknown scaling type: '%s'", val);
		break;
	}

	default:
		blog(LOG_WARNING, "load_gpu_scale_type: unknown data type: %d",
		     obs_data_item_gettype(item));
	}

	return default_scale_type;
}

static void adjust_video_encoder_scaling(const obs_video_info &ovi,
					 obs_encoder_t *video_encoder,
					 obs_data_t *encoder_config,
					 size_t encoder_index)
{
	uint64_t requested_width = obs_data_get_int(encoder_config, "width");
	uint64_t requested_height = obs_data_get_int(encoder_config, "height");

	if (ovi.output_width == requested_width ||
	    ovi.output_height == requested_height)
		return;

	if (ovi.base_width < requested_width ||
	    ovi.base_height < requested_height) {
		blog(LOG_WARNING,
		     "Requested resolution exceeds canvas/available resolution for encoder %zu: %" PRIu64
		     "x%" PRIu64 " > %" PRIu32 "x%" PRIu32,
		     encoder_index, requested_width, requested_height,
		     ovi.base_width, ovi.base_height);
	}

	obs_encoder_set_scaled_size(video_encoder,
				    static_cast<uint32_t>(requested_width),
				    static_cast<uint32_t>(requested_height));
	obs_encoder_set_gpu_scale_type(video_encoder,
				       load_gpu_scale_type(encoder_config));
}

static uint32_t closest_divisor(const obs_video_info &ovi,
				const media_frames_per_second &target_fps)
{
	auto target = (uint64_t)target_fps.numerator * ovi.fps_den;
	auto source = (uint64_t)ovi.fps_num * target_fps.denominator;
	return std::max(1u, static_cast<uint32_t>(source / target));
}

static void adjust_encoder_frame_rate_divisor(const obs_video_info &ovi,
					      obs_encoder_t *video_encoder,
					      obs_data_t *encoder_config,
					      const size_t encoder_index)
{
	media_frames_per_second requested_fps;
	const char *option = nullptr;
	if (!obs_data_get_frames_per_second(encoder_config, "framerate",
					    &requested_fps, &option)) {
		blog(LOG_WARNING, "`framerate` not specified for encoder %zu",
		     encoder_index);
		return;
	}

	if (ovi.fps_num == requested_fps.numerator &&
	    ovi.fps_den == requested_fps.denominator)
		return;

	auto divisor = closest_divisor(ovi, requested_fps);
	if (divisor <= 1)
		return;

	blog(LOG_INFO, "Setting frame rate divisor to %u for encoder %zu",
	     divisor, encoder_index);
	obs_encoder_set_frame_rate_divisor(video_encoder, divisor);
}

static const std::vector<const char *> &get_available_encoders()
{
	// encoders are currently only registered during startup, so keeping
	// a static vector around shouldn't be a problem
	static std::vector<const char *> available_encoders = [] {
		std::vector<const char *> available_encoders;
		for (size_t i = 0;; i++) {
			const char *id = nullptr;
			if (!obs_enum_encoder_types(i, &id))
				break;
			available_encoders.push_back(id);
		}
		return available_encoders;
	}();
	return available_encoders;
}

static bool encoder_available(const char *type)
{
	auto &encoders = get_available_encoders();
	return std::find_if(std::begin(encoders), std::end(encoders),
			    [=](const char *encoder) {
				    return strcmp(type, encoder) == 0;
			    }) != std::end(encoders);
}

static OBSEncoderAutoRelease create_video_encoder(DStr &name_buffer,
						  size_t encoder_index,
						  obs_data_t *encoder_config)
{
	auto encoder_type = obs_data_get_string(encoder_config, "type");
	if (!encoder_available(encoder_type)) {
		blog(LOG_ERROR, "Encoder type '%s' not available",
		     encoder_type);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.EncoderNotAvailable")
				.arg(encoder_type));
	}

	dstr_printf(name_buffer, "multitrack video video encoder %zu",
		    encoder_index);

	if (obs_data_has_user_value(encoder_config, "keyInt_sec") &&
	    !obs_data_has_user_value(encoder_config, "keyint_sec")) {
		blog(LOG_INFO,
		     "Fixing Go Live Config for encoder '%s': keyInt_sec -> keyint_sec",
		     name_buffer->array);
		obs_data_set_int(encoder_config, "keyint_sec",
				 obs_data_get_int(encoder_config,
						  "keyInt_sec"));
	}

	obs_data_set_bool(encoder_config, "disable_scenecut", true);

	OBSEncoderAutoRelease video_encoder = obs_video_encoder_create(
		encoder_type, name_buffer, encoder_config, nullptr);
	if (!video_encoder) {
		blog(LOG_ERROR, "failed to create video encoder '%s'",
		     name_buffer->array);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToCreateVideoEncoder")
				.arg(name_buffer->array, encoder_type));
	}
	obs_encoder_set_video(video_encoder, obs_get_video());

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING,
		     "Failed to get obs video info while creating encoder %zu",
		     encoder_index);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToGetOBSVideoInfo")
				.arg(name_buffer->array, encoder_type));
	}

	adjust_video_encoder_scaling(ovi, video_encoder, encoder_config,
				     encoder_index);
	adjust_encoder_frame_rate_divisor(ovi, video_encoder, encoder_config,
					  encoder_index);

	return video_encoder;
}

static OBSEncoderAutoRelease
create_audio_encoder(const char *audio_encoder_id,
		     std::optional<int> audio_bitrate)
{
	OBSDataAutoRelease settings = nullptr;
	if (audio_bitrate.has_value()) {
		settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", *audio_bitrate);
	}

	OBSEncoderAutoRelease audio_encoder = obs_audio_encoder_create(
		audio_encoder_id, "multitrack video aac", settings, 0, nullptr);
	if (!audio_encoder) {
		blog(LOG_ERROR, "failed to create audio encoder");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateAudioEncoder"));
	}
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	return audio_encoder;
}

struct OBSOutputs {
	OBSOutputAutoRelease output, recording_output;
};

static OBSOutputs
SetupOBSOutput(obs_data_t *dump_stream_to_file_config,
	       obs_data_t *go_live_config,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       OBSEncoderAutoRelease &audio_encoder,
	       const char *audio_encoder_id, std::optional<int> audio_bitrate);
static void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self,
				obs_output_t *output, OBSSignal &start,
				OBSSignal &stop, OBSSignal &deactivate);

struct OutputObjects {
	OBSOutputAutoRelease output;
	std::vector<OBSEncoderAutoRelease> video_encoders;
	OBSEncoderAutoRelease audio_encoder;
	OBSServiceAutoRelease multitrack_video_service;
};

struct ModuleHash {
	std::string Algorithm;
	std::string Hash;
	std::string Path;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ModuleHash, Algorithm, Hash, Path)
};

struct HashMismatch {
	const char *file_name;
	const char *current_hash;
	std::string expected_hash;
};

static void CheckPluginIntegrity(QWidget *parent)
{
	using json = nlohmann::json;

	DStr url;
	dstr_catf(url, "https://d50yg09cghihd.cloudfront.net/hashes/%s.json",
		  obs_get_version_string());

	std::string response;
	std::string error;
	long response_code = 0;
	if (!GetRemoteFile(url->array, response, error, &response_code, nullptr,
			   "GET", nullptr, {}, nullptr, 10)) {
		blog(LOG_WARNING,
		     "CheckPluginIntegrity: Failed to download hashes from '%s' (code %ld): %s",
		     url->array, response_code, error.c_str());
		return;
	}

	std::vector<HashMismatch> mismatches;

	try {
		auto data = json::parse(response);

		auto &hashes = data["/hashes"_json_pointer];
		std::optional<json::exception> exception;

		auto callback = [&](obs_module_t *module) {
			if (exception.has_value())
				return;

			auto file_name = obs_get_module_file_name(module);
			if (!file_name)
				return;

			auto hash = obs_get_module_hash_sha256(module);
			if (!hash)
				return;

			auto file_name_length = strlen(file_name);

			for (auto &entry : hashes)
				try {
					auto module_hash =
						entry.template get<ModuleHash>();

					if (qstrnicmp(module_hash.Algorithm
							      .c_str(),
						      "SHA256", 7) != 0)
						continue;

					if (module_hash.Path.size() <
					    file_name_length)
						continue;

					auto potential_file_name_offset =
						module_hash.Path.size() -
						file_name_length;

					auto potential_file_name_start =
						module_hash.Path.c_str() +
						potential_file_name_offset;
					if (qstrnicmp(potential_file_name_start,
						      file_name,
						      file_name_length) != 0)
						continue;

					if (qstrnicmp(module_hash.Hash.c_str(),
						      hash,
						      module_hash.Hash.size()) ==
					    0)
						return;

					mismatches.push_back(
						{file_name, hash,
						 module_hash.Hash});
					return;
				} catch (json::exception e) {
					exception.emplace(std::move(e));
				}
		};
		using callback_t = decltype(callback);

		obs_enum_modules(
			[](void *context, obs_module_t *module) {
				(*static_cast<callback_t *>(context))(module);
			},
			&callback);

		if (exception)
			throw *exception;
	} catch (const json::exception &exception) {
		blog(LOG_ERROR,
		     "CheckPluginIntegrity: Error while processing plugin integrity json (%d): %s",
		     exception.id, exception.what());
		return;
	}

	if (mismatches.empty())
		return;

	blog(LOG_ERROR,
	     "CheckPluginIntegrity: File hashes don't match expected values:");
	for (auto &mismatch : mismatches) {
		blog(LOG_ERROR,
		     "    %s: expected sha256: '%s' actual sha256: '%s'",
		     mismatch.file_name, mismatch.expected_hash.c_str(),
		     mismatch.current_hash);
	}

	bool ret = false;
	QMetaObject::invokeMethod(
		parent,
		[=] {
			QMessageBox mb(parent);
			mb.setIcon(QMessageBox::Warning);
			mb.setWindowTitle(QTStr("ModuleMismatchError.Title"));
			mb.setTextFormat(Qt::RichText);
			mb.setText(QTStr("ModuleMismatchError.Text"));
			mb.setStandardButtons(QMessageBox::StandardButton::Yes |
					      QMessageBox::StandardButton::No);
			return mb.exec() == QMessageBox::StandardButton::No;
		},
		BlockingConnectionTypeFor(parent), &ret);
	if (ret)
		throw MultitrackVideoError::cancel();
}

void MultitrackVideoOutput::PrepareStreaming(
	QWidget *parent, const char *service_name, obs_service_t *service,
	const std::optional<std::string> &rtmp_url, const QString &stream_key,
	const char *audio_encoder_id, int audio_bitrate,
	std::optional<uint32_t> maximum_aggregate_bitrate,
	std::optional<uint32_t> maximum_video_tracks,
	std::optional<std::string> custom_config,
	obs_data_t *dump_stream_to_file_config)
{
	if (!berryessa_) {
		QMetaObject::invokeMethod(
			parent,
			[&] {
				berryessa_ = std::make_unique<BerryessaSubmitter>(
					parent,
					"https://data.stats.live-video.net/");
				berryessa_->setAlwaysString("device_id",
							    device_id());
				berryessa_->setAlwaysString("obs_session_id",
							    obs_session_id());
			},
			BlockingConnectionTypeFor(parent));
	}

	if (berryessa_every_minute_ && berryessa_every_minute_->has_value()) {
		// destruct berryessa_every_minute on main thread
		QMetaObject::invokeMethod(
			parent, [bem = std::move(berryessa_every_minute_)] {});
	}

	if (current || current_stream_dump) {
		blog(LOG_WARNING,
		     "Tried to prepare multitrack video output while it's already active");
		return;
	}

	CheckPluginIntegrity(parent);

	if (!berryessa_every_minute_) {
		berryessa_every_minute_ =
			std::make_shared<std::optional<BerryessaEveryMinute>>(
				std::nullopt);
	}

	auto attempt_start_time = GenerateStreamAttemptStartTime();
	OBSDataAutoRelease go_live_post;
	OBSDataAutoRelease go_live_config;
	quint64 download_time_elapsed = 0;
	bool is_custom_config = custom_config.has_value();
	auto auto_config_url = MultitrackVideoAutoConfigURL(service);

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);
	auto multitrack_video_name =
		QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
	if (obs_data_has_user_value(service_settings,
				    "ertmp_multitrack_video_name")) {
		multitrack_video_name = obs_data_get_string(
			service_settings, "ertmp_multitrack_video_name");
	}

	auto auto_config_url_data = auto_config_url.toUtf8();

	blog(LOG_INFO,
	     "Preparing enhanced broadcasting stream for:\n"
	     "    device_id:      %s\n"
	     "    obs_session_id: %s\n"
	     "    custom config:  %s\n"
	     "    config url:     %s\n"
	     "  settings:\n"
	     "    service:               %s\n"
	     "    max aggregate bitrate: %s (%" PRIu32 ")\n"
	     "    max video tracks:      %s (%" PRIu32 ")\n"
	     "    custom rtmp url:       %s ('%s')",
	     device_id().toUtf8().constData(),
	     obs_session_id().toUtf8().constData(),
	     is_custom_config ? "Yes" : "No",
	     !auto_config_url.isEmpty() ? auto_config_url_data.constData()
					: "(null)",
	     service_name,
	     maximum_aggregate_bitrate.has_value() ? "Set" : "Auto",
	     maximum_aggregate_bitrate.value_or(0),
	     maximum_video_tracks.has_value() ? "Set" : "Auto",
	     maximum_video_tracks.value_or(0),
	     rtmp_url.has_value() ? "Yes" : "No",
	     rtmp_url.has_value() ? rtmp_url->c_str() : "");

	try {
		go_live_post = constructGoLivePost(attempt_start_time,
						   stream_key,
						   maximum_aggregate_bitrate,
						   maximum_video_tracks);

		go_live_config = DownloadGoLiveConfig(parent, auto_config_url,
						      go_live_post);
		if (!go_live_config)
			throw MultitrackVideoError::warning(
				QTStr("FailedToStartStream.FallbackToDefault")
					.arg(multitrack_video_name));

		download_time_elapsed = attempt_start_time.MSecsElapsed();

		if (custom_config.has_value()) {
			OBSDataAutoRelease custom = obs_data_create_from_json(
				custom_config->c_str());
			if (!custom)
				throw MultitrackVideoError::critical(QTStr(
					"FailedToStartStream.InvalidCustomConfig"));

			// copy unique ID from go live request
			OBSDataAutoRelease go_live_meta =
				obs_data_get_obj(go_live_config, "meta");
			auto uuid =
				obs_data_get_string(go_live_meta, "config_id");

			QByteArray generated_uuid_storage;

			if (!uuid || !uuid[0]) {
				QString generated_uuid =
					QUuid::createUuid().toString(
						QUuid::WithoutBraces);
				generated_uuid_storage =
					generated_uuid.toUtf8();
				uuid = generated_uuid_storage.constData();
				blog(LOG_INFO,
				     "Failed to copy config_id from go live config, using: %s",
				     uuid);
			} else {
				blog(LOG_INFO,
				     "Using config_id from go live config with custom config: %s",
				     uuid);
			}

			OBSDataAutoRelease meta =
				obs_data_get_obj(custom, "meta");
			if (!meta) {
				meta = obs_data_create();
				obs_data_set_obj(custom, "meta", meta);
			}
			obs_data_set_string(meta, "config_id", uuid);

			blog(LOG_INFO, "Using custom go live config: %s",
			     obs_data_get_json_pretty(custom));
			go_live_config = std::move(custom);
		}

		// Put the config_id (whether we created it or downloaded it) on all
		// Berryessa submissions from this point
		OBSDataAutoRelease goLiveMeta =
			obs_data_get_obj(go_live_config, "meta");
		if (goLiveMeta) {
			const char *s =
				obs_data_get_string(goLiveMeta, "config_id");
			blog(LOG_INFO, "Enhanced broadcasting config_id: '%s'",
			     s);
			if (s && *s && berryessa_) {
				add_always_string(berryessa_.get(), "config_id",
						  s);
			}
		}
		add_always_bool(berryessa_.get(), "config_custom",
				is_custom_config);

		auto video_encoders = std::vector<OBSEncoderAutoRelease>();
		OBSEncoderAutoRelease audio_encoder = nullptr;
		auto outputs = SetupOBSOutput(dump_stream_to_file_config,
					      go_live_config, video_encoders,
					      audio_encoder, audio_encoder_id,
					      audio_bitrate);
		auto output = std::move(outputs.output);
		auto recording_output = std::move(outputs.recording_output);
		if (!output)
			throw MultitrackVideoError::warning(
				QTStr("FailedToStartStream.FallbackToDefault")
					.arg(multitrack_video_name));

		std::vector<OBSEncoderAutoRelease> audio_encoders;
		audio_encoders.emplace_back(std::move(audio_encoder));

		auto multitrack_video_service =
			create_service(device_id(), obs_session_id(),
				       go_live_config, rtmp_url, stream_key);
		if (!multitrack_video_service)
			throw MultitrackVideoError::warning(
				QTStr("FailedToStartStream.FallbackToDefault")
					.arg(multitrack_video_name));

		obs_output_set_service(output, multitrack_video_service);

		OBSSignal start_streaming;
		OBSSignal stop_streaming;
		OBSSignal deactivate_stream;
		SetupSignalHandlers(false, this, output, start_streaming,
				    stop_streaming, deactivate_stream);

		if (dump_stream_to_file_config && recording_output) {
			OBSSignal start_recording;
			OBSSignal stop_recording;
			OBSSignal deactivate_recording;
			SetupSignalHandlers(true, this, recording_output,
					    start_recording, stop_recording,
					    deactivate_recording);

			decltype(video_encoders) recording_video_encoders;
			recording_video_encoders.reserve(video_encoders.size());
			for (auto &encoder : video_encoders) {
				recording_video_encoders.emplace_back(
					obs_encoder_get_ref(encoder));
			}

			decltype(audio_encoders) recording_audio_encoders;
			recording_audio_encoders.reserve(audio_encoders.size());
			for (auto &encoder : audio_encoders) {
				recording_audio_encoders.emplace_back(
					obs_encoder_get_ref(encoder));
			}

			current_stream_dump.emplace(OBSOutputObjects{
				std::move(recording_output),
				std::move(recording_video_encoders),
				std::move(recording_audio_encoders),
				nullptr,
				std::move(start_recording),
				std::move(stop_recording),
				std::move(deactivate_recording),
			});
		}

		current.emplace(OBSOutputObjects{
			std::move(output),
			std::move(video_encoders),
			std::move(audio_encoders),
			std::move(multitrack_video_service),
			std::move(start_streaming),
			std::move(stop_streaming),
			std::move(deactivate_stream),
		});

		if (berryessa_) {
			send_start_event = [berryessa = berryessa_.get(),
					    attempt_start_time,
					    download_time_elapsed,
					    is_custom_config,
					    go_live_post =
						    OBSData{go_live_post},
					    go_live_config =
						    OBSData{go_live_config}](
						   bool success,
						   std::optional<int>
							   connect_time_ms) {
				auto start_streaming_returned =
					attempt_start_time.MSecsElapsed();

				add_always_string(berryessa,
						  "stream_attempt_start_time",
						  attempt_start_time.CStr());

				if (!success) {
					auto event =
						MakeEvent_ivs_obs_stream_start_failed(
							go_live_post,
							go_live_config,
							attempt_start_time,
							download_time_elapsed,
							start_streaming_returned);
					submit_event(
						berryessa,
						"ivs_obs_stream_start_failed",
						event);
				} else {
					auto event =
						MakeEvent_ivs_obs_stream_start(
							go_live_post,
							go_live_config,
							attempt_start_time,
							download_time_elapsed,
							start_streaming_returned,
							connect_time_ms);

					submit_event(berryessa,
						     "ivs_obs_stream_start",
						     event);
				}
			};
		}
	} catch (...) {
		auto start_streaming_returned =
			attempt_start_time.MSecsElapsed();
		auto event = MakeEvent_ivs_obs_stream_start_failed(
			go_live_post, go_live_config, attempt_start_time,
			download_time_elapsed, start_streaming_returned);
		submit_event(berryessa_.get(), "ivs_obs_stream_start_failed",
			     event);

		if (berryessa_) {
			berryessa_->unsetAlways("config_id");
			berryessa_->unsetAlways("stream_attempt_start_time");
		}
		throw;
	}
}

signal_handler_t *MultitrackVideoOutput::StreamingSignalHandler()
{
	return current.has_value()
		       ? obs_output_get_signal_handler(current->output_)
		       : nullptr;
}

void MultitrackVideoOutput::StartedStreaming(QWidget *parent, bool success)
{
	if (!success) {
		if (send_start_event)
			send_start_event(false, std::nullopt);
		send_start_event = {};
		return;
	}

	if (current_stream_dump && current_stream_dump->output_) {
		auto result = obs_output_start(current_stream_dump->output_);
		blog(LOG_INFO, "MultitrackVideoOutput: starting recording%s",
		     result ? "" : " failed");
	}

	if (send_start_event)
		send_start_event(true, ConnectTimeMs());

	send_start_event = {};

	if (berryessa_ && current) {
		std::vector<OBSEncoder> video_encoders;
		video_encoders.reserve(current->video_encoders_.size());
		for (const auto &encoder : current->video_encoders_) {
			video_encoders.emplace_back(encoder);
		}

		berryessa_every_minute_initializer_.setFuture(
			CreateFuture().then(
				QThreadPool::globalInstance(),
				[=, bem = berryessa_every_minute_,
				 berryessa = berryessa_.get(),
				 main_thread = QThread::currentThread()] {
					bem->emplace(parent, berryessa,
						     video_encoders)
						.moveToThread(main_thread);
				}));
	}
}

void MultitrackVideoOutput::StopStreaming()
{
	if (current && current->output_) {
		obs_output_stop(current->output_);

		submit_event(berryessa_.get(), "ivs_obs_stream_stop",
			     MakeEvent_ivs_obs_stream_stop());
	}

	if (current_stream_dump && current_stream_dump->output_)
		obs_output_stop(current_stream_dump->output_);
}

std::optional<int> MultitrackVideoOutput::ConnectTimeMs() const
{
	if (!current || !current->output_)
		return std::nullopt;

	return obs_output_get_connect_time_ms(current->output_);
}

bool MultitrackVideoOutput::HandleIncompatibleSettings(
	QWidget *parent, config_t *config, obs_service_t *service,
	bool &useDelay, bool &enableNewSocketLoop, bool &enableDynBitrate)
{
	QString incompatible_settings;
	QString where_to_disable;
	QString incompatible_settings_list;

	size_t num = 1;

	auto check_setting = [&](bool setting, const char *name,
				 const char *section) {
		if (!setting)
			return;

		incompatible_settings +=
			QString(" %1. %2\n").arg(num).arg(QTStr(name));

		where_to_disable +=
			QString(" %1. [%2 > %3 > %4]\n")
				.arg(num)
				.arg(QTStr("Settings"))
				.arg(QTStr("Basic.Settings.Advanced"))
				.arg(QTStr(section));

		incompatible_settings_list += QString("%1, ").arg(name);

		num += 1;
	};

	check_setting(useDelay, "Basic.Settings.Advanced.StreamDelay",
		      "Basic.Settings.Advanced.StreamDelay");
	check_setting(enableNewSocketLoop,
		      "Basic.Settings.Advanced.Network.EnableNewSocketLoop",
		      "Basic.Settings.Advanced.Network");
	check_setting(enableDynBitrate,
		      "Basic.Settings.Output.DynamicBitrate.Beta",
		      "Basic.Settings.Advanced.Network");

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);

	QMessageBox mb(parent);
	mb.setIcon(QMessageBox::Critical);
	mb.setWindowTitle("Incompatible Settings");
	mb.setText(
		QString("%1 is not currently compatible with:\n\n%2\n"
			"To continue streaming with %1, disable "
			"incompatible settings:\n\n%3\n"
			"and Start Streaming again.")
			.arg(obs_data_get_string(service_settings,
						 "ertmp_multitrack_video_name"))
			.arg(incompatible_settings)
			.arg(where_to_disable));
	auto this_stream =
		mb.addButton("Disable for this stream and Start Streaming",
			     QMessageBox::AcceptRole);
	auto all_streams = mb.addButton(
		QString("Update %1 and Start Streaming").arg(QTStr("Settings")),
		QMessageBox::AcceptRole);
	mb.setStandardButtons(QMessageBox::StandardButton::Cancel);

	mb.exec();

	{
		QString action = "cancel";
		if (mb.clickedButton() == this_stream) {
			action = this_stream->text();
		} else if (mb.clickedButton() == all_streams) {
			action = all_streams->text();
		}

		auto error =
			QString("attempted to start stream with incompatible settings (%1); "
				"action taken: %2")
				.arg(incompatible_settings_list)
				.arg(action)
				.toUtf8();
		auto ev = MakeEvent_ivs_obs_stream_stop();
		obs_data_set_string(ev, "server_error", error.constData());
		submit_event(berryessa_.get(), "ivs_obs_stream_stop", ev);
		blog(LOG_INFO, "MultitrackVideoOutput: %s", error.constData());
	}

	if (mb.clickedButton() == this_stream ||
	    mb.clickedButton() == all_streams) {
		useDelay = false;
		enableNewSocketLoop = false;
		enableDynBitrate = false;
		useDelay = false;
		enableNewSocketLoop = false;
		enableDynBitrate = false;

		if (mb.clickedButton() == all_streams) {
			config_set_bool(config, "Output", "DelayEnable", false);
#ifdef _WIN32
			config_set_bool(config, "Output", "NewSocketLoopEnable",
					false);
#endif
			config_set_bool(config, "Output", "DynamicBitrate",
					false);
		}

		return true;
	}

	return false;
}

static OBSOutputs
SetupOBSOutput(obs_data_t *dump_stream_to_file_config,
	       obs_data_t *go_live_config,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       OBSEncoderAutoRelease &audio_encoder,
	       const char *audio_encoder_id, std::optional<int> audio_bitrate)
{

	auto output = create_output();
	OBSOutputAutoRelease recording_output;
	if (dump_stream_to_file_config)
		recording_output =
			create_recording_output(dump_stream_to_file_config);

	OBSDataArrayAutoRelease encoder_configs =
		obs_data_get_array(go_live_config, "encoder_configurations");
	DStr video_encoder_name_buffer;
	obs_encoder_t *first_encoder = nullptr;
	const size_t num_encoder_configs =
		obs_data_array_count(encoder_configs);
	if (num_encoder_configs < 1)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.MissingEncoderConfigs"));

	for (size_t i = 0; i < num_encoder_configs; i++) {
		OBSDataAutoRelease encoder_config =
			obs_data_array_item(encoder_configs, i);
		auto encoder = create_video_encoder(video_encoder_name_buffer,
						    i, encoder_config);
		if (!encoder)
			return {nullptr, nullptr};

		if (!first_encoder)
			first_encoder = encoder;
		else
			obs_encoder_group_keyframe_aligned_encoders(
				first_encoder, encoder);

		obs_output_set_video_encoder2(output, encoder, i);
		if (recording_output)
			obs_output_set_video_encoder2(recording_output, encoder,
						      i);
		video_encoders.emplace_back(std::move(encoder));
	}

	audio_encoder = create_audio_encoder(audio_encoder_id, audio_bitrate);
	obs_output_set_audio_encoder(output, audio_encoder, 0);
	if (recording_output)
		obs_output_set_audio_encoder(recording_output, audio_encoder,
					     0);

	return {std::move(output), std::move(recording_output)};
}

void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self,
			 obs_output_t *output, OBSSignal &start,
			 OBSSignal &stop, OBSSignal &deactivate)
{
	auto handler = obs_output_get_signal_handler(output);

	start.Connect(handler, "start",
		      !recording ? StreamStartHandler : RecordingStartHandler,
		      self);

	stop.Connect(handler, "stop",
		     !recording ? StreamStopHandler : RecordingStopHandler,
		     self);

	deactivate.Connect(handler, "deactivate",
			   !recording ? StreamDeactivateHandler
				      : RecordingDeactivateHandler,
			   self);
}

void StreamStartHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);

	if (!self->stream_attempt_start_time_.has_value() || !self->berryessa_)
		return;

	auto event = MakeEvent_ivs_obs_stream_started(
		self->stream_attempt_start_time_->MSecsElapsed());
	self->berryessa_->submit("ivs_obs_stream_started", event);
}

void StreamStopHandler(void *arg, calldata_t *params)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);

	if (self->current_stream_dump && self->current_stream_dump->output_) {
		obs_output_stop(self->current_stream_dump->output_);
	}

	auto code = calldata_int(params, "code");
	auto last_error = calldata_string(params, "last_error");

	auto stopped_event = MakeEvent_ivs_obs_stream_stopped(
		code == OBS_OUTPUT_SUCCESS ? nullptr : &code, last_error);

	decltype(self->current) object_releaser = std::nullopt;
	if (!obs_output_active(static_cast<obs_output_t *>(
		    calldata_ptr(params, "output")))) {
		object_releaser = std::move(self->current);
		self->current.reset();
	}

	QMetaObject::invokeMethod(
		QApplication::instance()->thread(),
		[berryessa = QPointer{self->berryessa_.get()},
		 bem = std::move(self->berryessa_every_minute_),
		 stopped_event = std::move(stopped_event),
		 object_releaser = std::move(object_releaser)] {
			submit_event(berryessa, "ivs_obs_stream_stopped",
				     stopped_event);

			if (berryessa) {
				berryessa->unsetAlways("config_id");
				berryessa->unsetAlways(
					"stream_attempt_start_time");
			}
		},
		Qt::QueuedConnection);

	self->berryessa_every_minute_ =
		std::make_shared<std::optional<BerryessaEveryMinute>>(
			std::nullopt);
}

void StreamDeactivateHandler(void *arg, calldata_t * /* data */)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	if (!self->current)
		return;

	QMetaObject::invokeMethod(QApplication::instance()->thread(),
				  [to_delete = std::move(self->current)] {});
	self->current.reset();
}

void RecordingStartHandler(void * /* arg */, calldata_t * /* data */)
{
	blog(LOG_INFO, "MultitrackVideoOutput: recording started");
}

void RecordingStopHandler(void *arg, calldata_t *params)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	blog(LOG_INFO, "MultitrackVideoOutput: recording stopped");

	if (!obs_output_active(static_cast<obs_output_t *>(
		    calldata_ptr(params, "output")))) {
		QMetaObject::invokeMethod(
			QApplication::instance()->thread(),
			[to_delete = std::move(self->current_stream_dump)] {});
		self->current_stream_dump.reset();
	}
}

void RecordingDeactivateHandler(void *arg, calldata_t * /*data*/)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	if (!self->current_stream_dump)
		return;

	QMetaObject::invokeMethod(
		QApplication::instance()->thread(),
		[to_delete = std::move(self->current_stream_dump)] {});
	self->current_stream_dump.reset();
}

const ImmutableDateTime &MultitrackVideoOutput::GenerateStreamAttemptStartTime()
{
	stream_attempt_start_time_.emplace(ImmutableDateTime::CurrentTimeUtc());
	return *stream_attempt_start_time_;
}
