/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs.h>
#include <stdio.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/array-serializer.h>
#include "flv-mux.h"
#include "obs-output-ver.h"
#include "rtmp-helpers.h"

/* TODO: FIXME: this is currently hard-coded to h264 and aac!  ..not that we'll
 * use anything else for a long time. */

//#define DEBUG_TIMESTAMPS
//#define WRITE_FLV_HEADER

#define VIDEO_HEADER_SIZE 5
#define VIDEODATA_AVCVIDEOPACKET 7.0
#define AUDIODATA_AAC 10.0

static inline double encoder_bitrate(obs_encoder_t *encoder)
{
	obs_data_t *settings = obs_encoder_get_settings(encoder);
	double bitrate = obs_data_get_double(settings, "bitrate");

	obs_data_release(settings);
	return bitrate;
}

#define FLV_INFO_SIZE_OFFSET 42

void write_file_info(FILE *file, int64_t duration_ms, int64_t size)
{
	char buf[64];
	char *enc = buf;
	char *end = enc + sizeof(buf);

	fseek(file, FLV_INFO_SIZE_OFFSET, SEEK_SET);

	enc_num_val(&enc, end, "duration", (double)duration_ms / 1000.0);
	enc_num_val(&enc, end, "fileSize", (double)size);

	fwrite(buf, 1, enc - buf, file);
}

static void build_flv_meta_data(obs_output_t *context, uint8_t **output,
				size_t *size)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(context);
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, 0);
	video_t *video = obs_encoder_video(vencoder);
	audio_t *audio = obs_encoder_audio(aencoder);
	char buf[4096];
	char *enc = buf;
	char *end = enc + sizeof(buf);
	struct dstr encoder_name = {0};

	enc_str(&enc, end, "@setDataFrame");
	enc_str(&enc, end, "onMetaData");

	*enc++ = AMF_ECMA_ARRAY;
	enc = AMF_EncodeInt32(enc, end, 20);

	enc_num_val(&enc, end, "duration", 0.0);
	enc_num_val(&enc, end, "fileSize", 0.0);

	enc_num_val(&enc, end, "width",
		    (double)obs_encoder_get_width(vencoder));
	enc_num_val(&enc, end, "height",
		    (double)obs_encoder_get_height(vencoder));

	enc_num_val(&enc, end, "videocodecid", VIDEODATA_AVCVIDEOPACKET);
	enc_num_val(&enc, end, "videodatarate", encoder_bitrate(vencoder));
	enc_num_val(&enc, end, "framerate", video_output_get_frame_rate(video));

	enc_num_val(&enc, end, "audiocodecid", AUDIODATA_AAC);
	enc_num_val(&enc, end, "audiodatarate", encoder_bitrate(aencoder));
	enc_num_val(&enc, end, "audiosamplerate",
		    (double)obs_encoder_get_sample_rate(aencoder));
	enc_num_val(&enc, end, "audiosamplesize", 16.0);
	enc_num_val(&enc, end, "audiochannels",
		    (double)audio_output_get_channels(audio));

	enc_bool_val(&enc, end, "stereo",
		     audio_output_get_channels(audio) == 2);
	enc_bool_val(&enc, end, "2.1", audio_output_get_channels(audio) == 3);
	enc_bool_val(&enc, end, "3.1", audio_output_get_channels(audio) == 4);
	enc_bool_val(&enc, end, "4.0", audio_output_get_channels(audio) == 4);
	enc_bool_val(&enc, end, "4.1", audio_output_get_channels(audio) == 5);
	enc_bool_val(&enc, end, "5.1", audio_output_get_channels(audio) == 6);
	enc_bool_val(&enc, end, "7.1", audio_output_get_channels(audio) == 8);

	dstr_printf(&encoder_name, "%s (libobs version ", MODULE_NAME);

#ifdef HAVE_OBSCONFIG_H
	dstr_cat(&encoder_name, obs_get_version_string());
#else
	dstr_catf(&encoder_name, "%d.%d.%d", LIBOBS_API_MAJOR_VER,
		  LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER);
#endif

	dstr_cat(&encoder_name, ")");

	enc_str_val(&enc, end, "encoder", encoder_name.array);
	dstr_free(&encoder_name);

	*enc++ = 0;
	*enc++ = 0;
	*enc++ = AMF_OBJECT_END;

	*size = enc - buf;
	*output = bmemdup(buf, *size);
}

static void write_previous_tag_size_without_header(struct serializer *s,
						   uint32_t header_size)
{
	assert(serializer_get_pos(s) >= header_size);
	assert(serializer_get_pos(s) >= 11);
	// From FLV file format specification version 10:
	// Size of previous [current] tag, including its header.
	// For FLV version 1 this value is 11 plus the DataSize of the previous [current] tag.
	s_wb32(s, (uint32_t)serializer_get_pos(s) - header_size);
}

static void write_previous_tag_size(struct serializer *s)
{
	write_previous_tag_size_without_header(s, 0);
}

void flv_meta_data(obs_output_t *context, uint8_t **output, size_t *size,
		   bool write_header)
{
	struct array_output_data data;
	struct serializer s;
	uint8_t *meta_data = NULL;
	size_t meta_data_size;
	uint32_t start_pos;

	array_output_serializer_init(&s, &data);
	build_flv_meta_data(context, &meta_data, &meta_data_size);

	if (write_header) {
		s_write(&s, "FLV", 3);
		s_w8(&s, 1);
		s_w8(&s, 5);
		s_wb32(&s, 9);
		s_wb32(&s, 0);
	}

	start_pos = serializer_get_pos(&s);

	s_w8(&s, RTMP_PACKET_TYPE_INFO);

	s_wb24(&s, (uint32_t)meta_data_size);
	s_wb32(&s, 0);
	s_wb24(&s, 0);

	s_write(&s, meta_data, meta_data_size);

	write_previous_tag_size_without_header(&s, start_pos);

	*output = data.bytes.array;
	*size = data.bytes.num;

	bfree(meta_data);
}

#ifdef DEBUG_TIMESTAMPS
static int32_t last_time = 0;
#endif

static void flv_video(struct serializer *s, int32_t dts_offset,
		      struct encoder_packet *packet, bool is_header)
{
	int64_t offset = packet->pts - packet->dts;
	int32_t time_ms = get_ms_time(packet, packet->dts) - dts_offset;

	if (!packet->data || !packet->size)
		return;

	s_w8(s, RTMP_PACKET_TYPE_VIDEO);

#ifdef DEBUG_TIMESTAMPS
	blog(LOG_DEBUG, "Video: %lu", time_ms);

	if (last_time > time_ms)
		blog(LOG_DEBUG, "Non-monotonic");

	last_time = time_ms;
#endif

	s_wb24(s, (uint32_t)packet->size + 5);
	s_wb24(s, time_ms);
	s_w8(s, (time_ms >> 24) & 0x7F);
	s_wb24(s, 0);

	/* these are the 5 extra bytes mentioned above */
	s_w8(s, packet->keyframe ? 0x17 : 0x27);
	s_w8(s, is_header ? 0 : 1);
	s_wb24(s, get_ms_time(packet, offset));
	s_write(s, packet->data, packet->size);

	write_previous_tag_size(s);
}

static void flv_audio(struct serializer *s, int32_t dts_offset,
		      struct encoder_packet *packet, bool is_header)
{
	int32_t time_ms = get_ms_time(packet, packet->dts) - dts_offset;

	if (!packet->data || !packet->size)
		return;

	s_w8(s, RTMP_PACKET_TYPE_AUDIO);

#ifdef DEBUG_TIMESTAMPS
	blog(LOG_DEBUG, "Audio: %lu", time_ms);

	if (last_time > time_ms)
		blog(LOG_DEBUG, "Non-monotonic");

	last_time = time_ms;
#endif

	s_wb24(s, (uint32_t)packet->size + 2);
	s_wb24(s, time_ms);
	s_w8(s, (time_ms >> 24) & 0x7F);
	s_wb24(s, 0);

	/* these are the two extra bytes mentioned above */
	s_w8(s, 0xaf);
	s_w8(s, is_header ? 0 : 1);
	s_write(s, packet->data, packet->size);

	write_previous_tag_size(s);
}

void flv_packet_mux(struct encoder_packet *packet, int32_t dts_offset,
		    uint8_t **output, size_t *size, bool is_header)
{
	struct array_output_data data;
	struct serializer s;

	array_output_serializer_init(&s, &data);

	if (packet->type == OBS_ENCODER_VIDEO)
		flv_video(&s, dts_offset, packet, is_header);
	else
		flv_audio(&s, dts_offset, packet, is_header);

	*output = data.bytes.array;
	*size = data.bytes.num;
}

/* ------------------------------------------------------------------------- */
/* stuff for additional media streams                                        */

#define s_amf_conststring(s, str)                   \
	do {                                        \
		const size_t len = sizeof(str) - 1; \
		s_wb16(s, (uint16_t)len);           \
		serialize(s, str, len);             \
	} while (false)

#define s_amf_string(s, str)                    \
	do {                                    \
		const size_t len = strlen(str); \
		s_wb16(s, (uint16_t)len);       \
		serialize(s, str, len);         \
	} while (false)

#define s_amf_double(s, d)                            \
	do {                                          \
		double d_val = d;                     \
		uint64_t u_val = *(uint64_t *)&d_val; \
		s_wb64(s, u_val);                     \
	} while (false)

static void flv_build_media_labels(struct serializer *s, struct darray *items)

{
	s_amf_conststring(s, "mediaLabels");
	s_w8(s, AMF_OBJECT);
	{
		flv_media_label_t *array = items->array;
		for (size_t i = 0; i < items->num; i++) {

			flv_media_label_t *item = array + i;
			s_amf_string(s, (char *)item->property);

			switch (item->type) {
			case FLV_MEDIA_LABEL_TYPE_NUMBER: {
				s_w8(s, AMF_NUMBER);
				s_amf_double(s,
					     flv_media_label_get_number(item));
				break;
			}
			case FLV_MEDIA_LABEL_TYPE_STRING: {
				s_w8(s, AMF_STRING);
				s_amf_string(s,
					     (char *)flv_media_label_get_string(
						     item));
				break;
			}
			default: {
			}
			}
		}
	}
	s_wb24(s, AMF_OBJECT_END);
}

static void
flv_build_additional_media_data_item(struct serializer *s,
				     flv_additional_media_data_t *md)
{
	s_amf_conststring(s, md->stream_name.array);

	s_w8(s, AMF_OBJECT);
	{
		s_amf_conststring(s, "type");

		s_w8(s, AMF_NUMBER);
		s_amf_double(s, md->type == OBS_ENCODER_AUDIO
					? RTMP_PACKET_TYPE_AUDIO
					: RTMP_PACKET_TYPE_VIDEO);

		/* ---- */

		flv_build_media_labels(s, &md->media_labels.da);
	}
	s_wb24(s, AMF_OBJECT_END);
}

static void flv_build_default_media_item_default_object(struct serializer *s,
							const char *type,
							struct darray *items)

{
	s_amf_string(s, (char *)type);
	s_w8(s, AMF_OBJECT);
	{
		flv_build_media_labels(s, items);
	}
	s_wb24(s, AMF_OBJECT_END);
}

static void flv_build_additional_meta_data(flv_additional_meta_data_t *amd,
					   uint8_t **data, size_t *size)
{
	struct array_output_data out;
	struct serializer s;

	array_output_serializer_init(&s, &out);

	s_w8(&s, AMF_STRING);
	s_amf_conststring(&s, "@setDataFrame");

	s_w8(&s, AMF_STRING);
	s_amf_conststring(&s, "onExpectAdditionalMedia");

	s_w8(&s, AMF_OBJECT);
	{
		s_amf_conststring(&s, "processingIntents");

		s_w8(&s, AMF_STRICT_ARRAY);
		s_wb32(&s, (uint32_t)amd->processing_intents.num);
		for (size_t i = 0; i < amd->processing_intents.num; i++) {
			s_w8(&s, AMF_STRING);
			s_amf_string(&s, amd->processing_intents.array[i]);
		}

		/* ---- */

		s_amf_conststring(&s, "additionalMedia");

		s_w8(&s, AMF_OBJECT);

		for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++) {
			if (amd->additional_audio_media_data[i].active)
				flv_build_additional_media_data_item(
					&s,
					&amd->additional_audio_media_data[i]);
		}
		for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			if (amd->additional_video_media_data[i].active)
				flv_build_additional_media_data_item(
					&s,
					&amd->additional_video_media_data[i]);
		}
		s_wb24(&s, AMF_OBJECT_END);

		/* ---- */

		s_amf_conststring(&s, "defaultMedia");

		s_w8(&s, AMF_OBJECT);
		{
			if (amd->default_audio_media_labels.num > 0)
				flv_build_default_media_item_default_object(
					&s, "audio",
					&amd->default_audio_media_labels.da);

			/* ---- */

			if (amd->default_video_media_labels.num > 0)
				flv_build_default_media_item_default_object(
					&s, "video",
					&amd->default_video_media_labels.da);
		}
		s_wb24(&s, AMF_OBJECT_END);
	}
	s_wb24(&s, AMF_OBJECT_END);

	*data = out.bytes.array;
	*size = out.bytes.num;
}

void flv_additional_meta_data(obs_output_t *context,
			      flv_additional_meta_data_t *additional_meta_data,
			      uint8_t **data, size_t *size)
{
	UNUSED_PARAMETER(context);
	struct array_output_data out;
	struct serializer s;
	uint8_t *meta_data = NULL;
	size_t meta_data_size;

	flv_build_additional_meta_data(additional_meta_data, &meta_data,
				       &meta_data_size);

	array_output_serializer_init(&s, &out);

	s_w8(&s, RTMP_PACKET_TYPE_INFO); //18

	s_wb24(&s, (uint32_t)meta_data_size);
	s_wb32(&s, 0);
	s_wb24(&s, 0);

	s_write(&s, meta_data, meta_data_size);
	bfree(meta_data);

	s_wb32(&s, (uint32_t)serializer_get_pos(&s));

	*data = out.bytes.array;
	*size = out.bytes.num;
}

static inline void s_u29(struct serializer *s, uint32_t val)
{
	if (val <= 0x7F) {
		s_w8(s, val);
	} else if (val <= 0x3FFF) {
		s_w8(s, 0x80 | (val >> 7));
		s_w8(s, val & 0x7F);
	} else if (val <= 0x1FFFFF) {
		s_w8(s, 0x80 | (val >> 14));
		s_w8(s, 0x80 | ((val >> 7) & 0x7F));
		s_w8(s, val & 0x7F);
	} else {
		s_w8(s, 0x80 | (val >> 22));
		s_w8(s, 0x80 | ((val >> 15) & 0x7F));
		s_w8(s, 0x80 | ((val >> 8) & 0x7F));
		s_w8(s, val & 0xFF);
	}
}

static inline void s_u29b_value(struct serializer *s, uint32_t val)
{
	s_u29(s, 1 | ((val & 0xFFFFFFF) << 1));
}

static void
flv_build_additional_media_data(uint8_t **data, size_t *size,
				struct encoder_packet *packet, bool is_header,
				flv_additional_media_data_t *media_data)
{
	struct array_output_data out;
	struct serializer s;

	array_output_serializer_init(&s, &out);

	s_w8(&s, AMF_STRING);
	s_amf_conststring(&s, "additionalMedia");

	s_w8(&s, AMF_OBJECT);
	{
		s_amf_conststring(&s, "id");

		s_w8(&s, AMF_STRING);
		s_amf_string(&s, media_data->stream_name.array);

		/* ----- */

		s_amf_conststring(&s, "media");

		s_w8(&s, AMF_AVMPLUS);
		s_w8(&s, AMF3_BYTE_ARRAY);

		if (packet->type == OBS_ENCODER_AUDIO) {
			s_u29b_value(&s, (uint32_t)packet->size + 2);
			s_w8(&s, 0xaf);
			s_w8(&s, is_header ? 0 : 1);
		} else {
			int64_t offset = packet->pts - packet->dts;
			/* these are the 5 extra bytes mentioned above */
			s_u29b_value(&s, (uint32_t)packet->size + 5);
			s_w8(&s, packet->keyframe ? 0x17 : 0x27);
			s_w8(&s, is_header ? 0 : 1);
			s_wb24(&s, get_ms_time(packet, offset));
		}
		s_write(&s, packet->data, packet->size);
	}
	s_wb24(&s, AMF_OBJECT_END);

	*data = out.bytes.array;
	*size = out.bytes.num;
}

static void flv_additional_stream(struct serializer *s, int32_t dts_offset,
				  struct encoder_packet *packet, bool is_header,
				  flv_additional_media_data_t *media_data)
{
	int32_t time_ms = get_ms_time(packet, packet->dts) - dts_offset;
	uint8_t *data;
	size_t size;

	if (!packet->data || !packet->size)
		return;

	flv_build_additional_media_data(&data, &size, packet, is_header,
					media_data);

	s_w8(s, RTMP_PACKET_TYPE_INFO); //18

#ifdef DEBUG_TIMESTAMPS
	blog(LOG_DEBUG, "Audio2: %lu", time_ms);

	if (last_time > time_ms)
		blog(LOG_DEBUG, "Non-monotonic");

	last_time = time_ms;
#endif

	s_wb24(s, (uint32_t)size);
	s_wb24(s, time_ms);
	s_w8(s, (time_ms >> 24) & 0x7F);
	s_wb24(s, 0);

	serialize(s, data, size);
	bfree(data);

	write_previous_tag_size(s);
}

void flv_additional_packet_mux(struct encoder_packet *packet,
			       int32_t dts_offset, uint8_t **data, size_t *size,
			       bool is_header,
			       flv_additional_media_data_t *media_data)
{
	struct array_output_data out;
	struct serializer s;

	array_output_serializer_init(&s, &out);

	flv_additional_stream(&s, dts_offset, packet, is_header, media_data);

	*data = out.bytes.array;
	*size = out.bytes.num;
}