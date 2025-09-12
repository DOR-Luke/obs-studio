/******************************************************************************
    Copyright (C) 2015 by Hugh Bailey <obs.jim@gmail.com>

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
#include "ffmpeg-mux/ffmpeg-mux.h"
#include "obs-ffmpeg-mux.h"
#include "obs-ffmpeg-formats.h"

#ifdef _WIN32
#include "util/windows/win-version.h"
#endif

#include <libavformat/avformat.h>
#include <time.h>
#include <stdlib.h>

/* Disk buffer packet header structure */
struct disk_packet_header {
	uint32_t magic;        /* Magic number for validation */
	uint32_t size;         /* Packet data size */
	uint32_t type;         /* Packet type (video/audio) */
	uint64_t dts_usec;     /* DTS timestamp in microseconds */
	int64_t pts;           /* Original PTS value */
	int64_t dts;           /* Original DTS value */
	uint32_t timebase_den; /* Original timebase denominator */
	uint32_t keyframe;     /* Is keyframe? */
	uint32_t track_idx;    /* Track index */
	uint32_t reserved;     /* Reserved for future use */
};

#define DISK_PACKET_MAGIC 0x4F425344  /* "OBSD" */

#define do_log(level, format, ...)                  \
	blog(level, "[ffmpeg muxer: '%s'] " format, \
	     obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

static const char *ffmpeg_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegMuxer");
}

#ifndef NEW_MPEGTS_OUTPUT
static const char *ffmpeg_mpegts_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegMpegtsMuxer");
}
#endif

/* Check if disk buffer mode is enabled - FORCED TO TRUE FOR TESTING */
static bool is_disk_buffer_mode_enabled(obs_output_t *output)
{
	UNUSED_PARAMETER(output);
	
	/* FORCE DISK MODE FOR TESTING */
	blog(LOG_INFO, "[Replay Buffer] FORCED DISK MODE FOR TESTING");
	return true;
}

/* Forward declaration for create_new_segment */
static bool create_new_segment(struct ffmpeg_muxer *stream);

/* Initialize segment-based disk buffer */
static bool init_segment_disk_buffer(struct ffmpeg_muxer *stream)
{
	if (stream->current_segment_file)
		return true;

	/* Get the replay buffer output directory from settings */
	obs_data_t *settings = obs_output_get_settings(stream->output);
	const char *output_dir = obs_data_get_string(settings, "directory");
	
	const char *temp_dir = output_dir;
	if (!temp_dir || !*temp_dir) {
		temp_dir = ".";
	}
	
	/* Create segments directory */
	char segments_dir[512];
	snprintf(segments_dir, sizeof(segments_dir), 
		"%s/obs_replay_segments", temp_dir);
	
	/* Create directory if it doesn't exist */
	os_mkdirs(segments_dir);
	
	dstr_copy(&stream->disk_buffer_dir, segments_dir);
	
	/* Initialize segment parameters */
	stream->current_segment_id = 0;
	stream->max_segments = (int)(stream->max_time / 10000000LL) + 1; /* 10 seconds per segment for testing */
	if (stream->max_segments < 2) stream->max_segments = 2;
	stream->segment_duration_usec = 10 * 1000000LL; /* 10 seconds per segment for testing */
	stream->segment_start_time = 0;
	
	info("Segment disk buffer initialized: max_time=%d max_segments=%d, segment_duration=%lld seconds", 
		(int)(stream->max_time / 1000000LL), stream->max_segments, stream->segment_duration_usec / 1000000LL);
	
	obs_data_release(settings);
	
	/* Create first segment */
	return create_new_segment(stream);
}

/* Create a new segment file */
static bool create_new_segment(struct ffmpeg_muxer *stream)
{
	/* Close current segment if exists */
	if (stream->current_segment_file) {
		fclose(stream->current_segment_file);
		stream->current_segment_file = NULL;
	}
	
	/* Remove old segment if we exceed max_segments */
	if (stream->current_segment_id >= stream->max_segments) {
		char old_segment[512];
		snprintf(old_segment, sizeof(old_segment),
			"%s/segment_%03d.tmp",
			stream->disk_buffer_dir.array,
			stream->current_segment_id - stream->max_segments);
		
		remove(old_segment);
		info("Removed old segment: %s", old_segment);
	}
	
	/* Create new segment file using CURRENT segment_id */
	char new_segment[512];
	snprintf(new_segment, sizeof(new_segment),
		"%s/segment_%03d.tmp",
		stream->disk_buffer_dir.array,
		stream->current_segment_id);
	
	stream->current_segment_file = fopen(new_segment, "wb");
	if (!stream->current_segment_file) {
		warn("Failed to create segment: %s", new_segment);
		return false;
	}
	
	info("Created new segment: segment_%03d.tmp (current_segment_id=%d)", 
		stream->current_segment_id, stream->current_segment_id);
	
	/* INCREMENT ID AFTER successful creation */
	stream->current_segment_id++;
	stream->segment_start_time = 0; /* Will be set on first packet */
	
	return true;
}

/* Store packet to segment-based disk buffer */
static void store_packet_to_disk(struct ffmpeg_muxer *stream, 
                                struct encoder_packet *packet)
{
	if (!init_segment_disk_buffer(stream))
		return;

	/* Check if we need to start a new segment based on time */
	if (stream->segment_start_time == 0) {
		stream->segment_start_time = packet->dts_usec;
		info("First packet stored, segment_start_time set to %lld", stream->segment_start_time);
	} else {
		int64_t segment_duration = packet->dts_usec - stream->segment_start_time;
		if (segment_duration > stream->segment_duration_usec) {
			/* Time to create a new segment */
			info("Creating new segment: duration %lld usec > %lld usec", 
				segment_duration, stream->segment_duration_usec);
			if (!create_new_segment(stream))
				return;
			stream->segment_start_time = packet->dts_usec;
		}
	}

	struct disk_packet_header header = {
		.magic = DISK_PACKET_MAGIC,
		.size = (uint32_t)packet->size,
		.type = (uint32_t)packet->type,
		.dts_usec = packet->dts_usec,
		.pts = packet->pts,
		.dts = packet->dts,
		.timebase_den = packet->timebase_den,
		.keyframe = packet->keyframe ? 1 : 0,
		.track_idx = (uint32_t)packet->track_idx,
		.reserved = 0
	};
	
	/* Write header to current segment */
	if (fwrite(&header, sizeof(header), 1, stream->current_segment_file) != 1) {
		warn("Failed to write packet header to segment");
		return;
	}
	
	/* Write packet data to current segment */
	if (fwrite(packet->data, packet->size, 1, stream->current_segment_file) != 1) {
		warn("Failed to write packet data to segment");
		return;
	}
	
	fflush(stream->current_segment_file);
	
	stream->disk_packet_count++;
	stream->disk_buffer_size += sizeof(header) + packet->size;
}

/* Remove oldest segment from disk buffer - TRUE front purging like RAM! */
static bool disk_buffer_purge_front(struct ffmpeg_muxer *stream)
{
	if (stream->keyframes <= 2)
		return false;
	
	/* Find the oldest existing segment to remove, but PROTECT current segment */
	int start_search = stream->current_segment_id - stream->max_segments;
	if (start_search < 0) start_search = 0;
	
	/* NEVER delete the current segment being written to */
	int end_search = stream->current_segment_id - 1;
	if (end_search < 0) {
		info("No segments available to purge (current_segment_id=%d)", stream->current_segment_id);
		return false;
	}
	
	for (int seg_id = start_search; seg_id <= end_search; seg_id++) {
		char segment_file[512];
		snprintf(segment_file, sizeof(segment_file),
			"%s/segment_%03d.tmp",
			stream->disk_buffer_dir.array, seg_id);
		
		/* Check if file exists */
		FILE *test_file = fopen(segment_file, "rb");
		if (test_file) {
			fclose(test_file);
			
			/* Remove this oldest existing segment */
			if (remove(segment_file) == 0) {
				info("Purged oldest existing segment: segment_%03d.tmp", seg_id);
				stream->keyframes--; /* Approximate keyframe reduction */
				return true;
			} else {
				warn("Failed to remove segment: segment_%03d.tmp", seg_id);
				return false;
			}
		}
	}
	
	info("No segments found to purge (searched %d to %d, protecting current segment %d)", 
		start_search, end_search, stream->current_segment_id - 1);
	return false;
}

/* Disk buffer purge - MODIFIED for segment-based approach */
static void disk_buffer_replay_purge(struct ffmpeg_muxer *stream, struct encoder_packet *pkt)
{
	/* For segment-based disk buffer, we purge more conservatively */
	if (stream->keyframes <= 2)
		return;

	/* Size-based purge: only if significantly over limit */
	if (stream->max_size) {
		if ((stream->cur_size + (int64_t)pkt->size) > (stream->max_size * 1.5)) {
			info("Size-based purge triggered: %lld > %lld", 
				stream->cur_size, stream->max_size);
			disk_buffer_purge_front(stream);
		}
	}

	/* Time-based purge: only if significantly over limit */
	if ((pkt->dts_usec - stream->cur_time) > (stream->max_time * 1.2)) {
		info("Time-based purge triggered: %lld > %lld", 
			(pkt->dts_usec - stream->cur_time), stream->max_time);
		disk_buffer_purge_front(stream);
	}
}

/* Clean up segment-based disk buffer */
static void cleanup_disk_buffer(struct ffmpeg_muxer *stream)
{
	if (stream->current_segment_file) {
		fclose(stream->current_segment_file);
		stream->current_segment_file = NULL;
	}
	
	/* Remove all segment files */
	if (stream->disk_buffer_dir.array) {
		for (int i = 0; i < stream->current_segment_id; i++) {
			char segment_file[512];
			snprintf(segment_file, sizeof(segment_file),
				"%s/segment_%03d.tmp",
				stream->disk_buffer_dir.array, i);
			remove(segment_file);
		}
		
		/* Remove segments directory if empty */
		os_rmdir(stream->disk_buffer_dir.array);
		dstr_free(&stream->disk_buffer_dir);
	}
	
	stream->current_segment_id = 0;
	stream->disk_packet_count = 0;
	stream->disk_buffer_size = 0;
}

/* Load packets from all segments for muxing */
static bool load_packets_from_disk_for_mux(struct ffmpeg_muxer *stream)
{
	if (!stream->current_segment_file || !stream->disk_buffer_dir.array)
		return false;

	/* Clear existing mux packets */
	for (size_t i = 0; i < stream->mux_packets.num; i++)
		obs_encoder_packet_release(&stream->mux_packets.array[i]);
	da_free(stream->mux_packets);

	/* Read packets from all existing segments in order */
	int start_segment = stream->current_segment_id - stream->max_segments;
	if (start_segment < 0) start_segment = 0;
	
	info("Loading segments for mux: start_segment=%d, current_segment_id=%d", 
		start_segment, stream->current_segment_id);
	
	/* Read all segments up to (but not including) current_segment_id
	   since current_segment_id points to the NEXT segment to be created */
	int end_segment = stream->current_segment_id - 1;
	
	info("Reading segments from %d to %d (current_segment_id=%d)", 
		start_segment, end_segment, stream->current_segment_id);
	
	for (int seg_id = start_segment; seg_id <= end_segment; seg_id++) {
		char segment_file[512];
		snprintf(segment_file, sizeof(segment_file),
			"%s/segment_%03d.tmp",
			stream->disk_buffer_dir.array, seg_id);
		
		FILE *read_file = fopen(segment_file, "rb");
		if (!read_file) {
			warn("Failed to open segment file: %s", segment_file);
			continue; /* Skip missing segments */
		}
		
		info("Reading packets from segment: %s", segment_file);
		
		struct disk_packet_header header;
		struct encoder_packet packet;
		
		while (fread(&header, sizeof(header), 1, read_file) == 1) {
			if (header.magic != DISK_PACKET_MAGIC) {
				warn("Invalid packet magic in segment %d", seg_id);
				break;
			}
			
			/* Allocate packet data */
			uint8_t *data = bmalloc(header.size);
			if (fread(data, header.size, 1, read_file) != 1) {
				bfree(data);
				warn("Failed to read packet data from segment %d", seg_id);
				break;
			}
			
			/* Create encoder packet - restore ALL original values */
			memset(&packet, 0, sizeof(packet));
			packet.data = data;
			packet.size = header.size;
			packet.type = (enum obs_encoder_type)header.type;
			packet.dts_usec = header.dts_usec;
			packet.pts = header.pts;
			packet.dts = header.dts;
			packet.timebase_den = header.timebase_den;
			packet.keyframe = header.keyframe != 0;
			packet.track_idx = (size_t)header.track_idx;
			
			/* Add to mux packets array */
			da_push_back(stream->mux_packets, &packet);
		}
		
		fclose(read_file);
	}
	
	info("Loaded %lu packets from %d segments for muxing", 
		(unsigned long)stream->mux_packets.num,
		stream->current_segment_id - start_segment);
	
	return stream->mux_packets.num > 0;
}

static inline void replay_buffer_clear(struct ffmpeg_muxer *stream)
{
	if (stream->use_disk_buffer) {
		cleanup_disk_buffer(stream);
	} else {
		while (stream->packets.size > 0) {
			struct encoder_packet pkt;
			circlebuf_pop_front(&stream->packets, &pkt, sizeof(pkt));
			obs_encoder_packet_release(&pkt);
		}
		circlebuf_free(&stream->packets);
	}

	stream->cur_size = 0;
	stream->cur_time = 0;
	stream->max_size = 0;
	stream->max_time = 0;
	stream->save_ts = 0;
	stream->keyframes = 0;
}

static void ffmpeg_mux_destroy(void *data)
{
	struct ffmpeg_muxer *stream = data;

	replay_buffer_clear(stream);
	if (stream->mux_thread_joinable)
		pthread_join(stream->mux_thread, NULL);
	for (size_t i = 0; i < stream->mux_packets.num; i++)
		obs_encoder_packet_release(&stream->mux_packets.array[i]);
	da_free(stream->mux_packets);
	circlebuf_free(&stream->packets);

	os_process_pipe_destroy(stream->pipe);
	dstr_free(&stream->path);
	dstr_free(&stream->printable_path);
	dstr_free(&stream->stream_key);
	dstr_free(&stream->muxer_settings);
	bfree(stream);
}

static void split_file_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;

	calldata_set_bool(cd, "split_file_enabled", stream->split_file);
	if (!stream->split_file)
		return;

	os_atomic_set_bool(&stream->manual_split, true);
}

static void *ffmpeg_mux_create(obs_data_t *settings, obs_output_t *output)
{
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	stream->output = output;

	if (obs_output_get_flags(output) & OBS_OUTPUT_SERVICE)
		stream->is_network = true;

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_add(sh, "void file_changed(string next_file)");

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_add(ph, "void split_file(out bool split_file_enabled)",
			 split_file_proc, stream);

	UNUSED_PARAMETER(settings);
	return stream;
}

#ifdef _WIN32
#define FFMPEG_MUX "obs-ffmpeg-mux.exe"
#else
#define FFMPEG_MUX "obs-ffmpeg-mux"
#endif

static inline bool capturing(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->capturing);
}

bool stopping(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->stopping);
}

bool active(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->active);
}

/* TODO: allow codecs other than h264 whenever we start using them */

static void add_video_encoder_params(struct ffmpeg_muxer *stream,
				     struct dstr *cmd, obs_encoder_t *vencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(vencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	video_t *video = obs_get_video();
	const struct video_output_info *info = video_output_get_info(video);

	int codec_tag = (int)obs_data_get_int(settings, "codec_type");
#if __BYTE_ORDER == __LITTLE_ENDIAN
	codec_tag = ((codec_tag >> 24) & 0x000000FF) |
		    ((codec_tag << 8) & 0x00FF0000) |
		    ((codec_tag >> 8) & 0x0000FF00) |
		    ((codec_tag << 24) & 0xFF000000);
#endif

	obs_data_release(settings);

	enum AVColorPrimaries pri = AVCOL_PRI_UNSPECIFIED;
	enum AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
	enum AVColorSpace spc = AVCOL_SPC_UNSPECIFIED;
	switch (info->colorspace) {
	case VIDEO_CS_601:
		pri = AVCOL_PRI_SMPTE170M;
		trc = AVCOL_TRC_SMPTE170M;
		spc = AVCOL_SPC_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		pri = AVCOL_PRI_BT709;
		trc = AVCOL_TRC_BT709;
		spc = AVCOL_SPC_BT709;
		break;
	case VIDEO_CS_SRGB:
		pri = AVCOL_PRI_BT709;
		trc = AVCOL_TRC_IEC61966_2_1;
		spc = AVCOL_SPC_BT709;
		break;
	case VIDEO_CS_2100_PQ:
		pri = AVCOL_PRI_BT2020;
		trc = AVCOL_TRC_SMPTE2084;
		spc = AVCOL_SPC_BT2020_NCL;
		break;
	case VIDEO_CS_2100_HLG:
		pri = AVCOL_PRI_BT2020;
		trc = AVCOL_TRC_ARIB_STD_B67;
		spc = AVCOL_SPC_BT2020_NCL;
	}

	const enum AVColorRange range = (info->range == VIDEO_RANGE_FULL)
						? AVCOL_RANGE_JPEG
						: AVCOL_RANGE_MPEG;

	const int max_luminance =
		(trc == AVCOL_TRC_SMPTE2084)
			? (int)obs_get_video_hdr_nominal_peak_level()
			: ((trc == AVCOL_TRC_ARIB_STD_B67) ? 1000 : 0);

	dstr_catf(cmd, "%s %d %d %d %d %d %d %d %d %d %d %d %d ",
		  obs_encoder_get_codec(vencoder), bitrate,
		  obs_output_get_width(stream->output),
		  obs_output_get_height(stream->output), (int)pri, (int)trc,
		  (int)spc, (int)range,
		  (int)determine_chroma_location(
			  obs_to_ffmpeg_video_format(info->format), spc),
		  max_luminance, (int)info->fps_num, (int)info->fps_den,
		  (int)codec_tag);
}

static void add_audio_encoder_params(struct dstr *cmd, obs_encoder_t *aencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(aencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	audio_t *audio = obs_get_audio();
	struct dstr name = {0};

	obs_data_release(settings);

	dstr_copy(&name, obs_encoder_get_name(aencoder));
	dstr_replace(&name, "\"", "\"\"");

	dstr_catf(cmd, "\"%s\" %d %d %d %d ", name.array, bitrate,
		  (int)obs_encoder_get_sample_rate(aencoder),
		  (int)obs_encoder_get_frame_size(aencoder),
		  (int)audio_output_get_channels(audio));

	dstr_free(&name);
}

static void log_muxer_params(struct ffmpeg_muxer *stream, const char *settings)
{
	int ret;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, settings, "=", " ", 0))) {
		warn("Failed to parse muxer settings: %s\n%s", av_err2str(ret),
		     settings);

		av_dict_free(&dict);
		return;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
					    AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		info("Using muxer settings:%s", str.array);
		dstr_free(&str);
	}

	av_dict_free(&dict);
}

static void add_stream_key(struct dstr *cmd, struct ffmpeg_muxer *stream)
{
	dstr_catf(cmd, "\"%s\" ",
		  dstr_is_empty(&stream->stream_key)
			  ? ""
			  : stream->stream_key.array);
}

static void add_muxer_params(struct dstr *cmd, struct ffmpeg_muxer *stream)
{
	struct dstr mux = {0};

	if (dstr_is_empty(&stream->muxer_settings)) {
		obs_data_t *settings = obs_output_get_settings(stream->output);
		dstr_copy(&mux,
			  obs_data_get_string(settings, "muxer_settings"));
		obs_data_release(settings);
	} else {
		dstr_copy(&mux, stream->muxer_settings.array);
	}

	log_muxer_params(stream, mux.array);

	dstr_replace(&mux, "\"", "\\\"");

	dstr_catf(cmd, "\"%s\" ", mux.array ? mux.array : "");

	dstr_free(&mux);
}

static void build_command_line(struct ffmpeg_muxer *stream, struct dstr *cmd,
			       const char *path)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	obs_encoder_t *aencoders[MAX_AUDIO_MIXES];
	int num_tracks = 0;

	for (;;) {
		obs_encoder_t *aencoder = obs_output_get_audio_encoder(
			stream->output, num_tracks);
		if (!aencoder)
			break;

		aencoders[num_tracks] = aencoder;
		num_tracks++;
	}

	dstr_init_move_array(cmd, os_get_executable_path_ptr(FFMPEG_MUX));
	dstr_insert_ch(cmd, 0, '\"');
	dstr_cat(cmd, "\" \"");

	dstr_copy(&stream->path, path);
	dstr_replace(&stream->path, "\"", "\"\"");
	dstr_cat_dstr(cmd, &stream->path);

	dstr_catf(cmd, "\" %d %d ", vencoder ? 1 : 0, num_tracks);

	if (vencoder)
		add_video_encoder_params(stream, cmd, vencoder);

	if (num_tracks) {
		dstr_cat(cmd, "aac ");

		for (int i = 0; i < num_tracks; i++) {
			add_audio_encoder_params(cmd, aencoders[i]);
		}
	}

	add_stream_key(cmd, stream);
	add_muxer_params(cmd, stream);
}

void start_pipe(struct ffmpeg_muxer *stream, const char *path)
{
	struct dstr cmd;
	build_command_line(stream, &cmd, path);
	stream->pipe = os_process_pipe_create(cmd.array, "w");
	dstr_free(&cmd);
}

static void set_file_not_readable_error(struct ffmpeg_muxer *stream,
					obs_data_t *settings, const char *path)
{
	struct dstr error_message;
	dstr_init_copy(&error_message, obs_module_text("UnableToWritePath"));
#ifdef _WIN32
	/* special warning for Windows 10 users about Defender */
	struct win_version_info ver;
	get_win_ver(&ver);
	if (ver.major >= 10) {
		dstr_cat(&error_message, "\n\n");
		dstr_cat(&error_message,
			 obs_module_text("WarnWindowsDefender"));
	}
#endif
	dstr_replace(&error_message, "%1", path);
	obs_output_set_last_error(stream->output, error_message.array);
	dstr_free(&error_message);
	obs_data_release(settings);
}

inline static void ts_offset_clear(struct ffmpeg_muxer *stream)
{
	stream->found_video = false;
	stream->video_pts_offset = 0;

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		stream->found_audio[i] = false;
		stream->audio_dts_offsets[i] = 0;
	}
}

static inline int64_t packet_pts_usec(struct encoder_packet *packet)
{
	return packet->pts * 1000000 / packet->timebase_den;
}

inline static void ts_offset_update(struct ffmpeg_muxer *stream,
				    struct encoder_packet *packet)
{
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (!stream->found_video) {
			stream->video_pts_offset = packet->pts;
			stream->found_video = true;
		}
		return;
	}

	if (stream->found_audio[packet->track_idx])
		return;

	stream->audio_dts_offsets[packet->track_idx] = packet->dts;
	stream->found_audio[packet->track_idx] = true;
}

static inline void update_encoder_settings(struct ffmpeg_muxer *stream,
					   const char *path)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	const char *ext = strrchr(path, '.');

	/* if using m3u8, repeat headers */
	if (ext && strcmp(ext, ".m3u8") == 0) {
		obs_data_t *settings = obs_encoder_get_settings(vencoder);
		obs_data_set_bool(settings, "repeat_headers", true);
		obs_encoder_update(vencoder, settings);
		obs_data_release(settings);
	}
}

static inline bool ffmpeg_mux_start_internal(struct ffmpeg_muxer *stream,
					     obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "path");

	update_encoder_settings(stream, path);

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	if (stream->is_network) {
		obs_service_t *service;
		service = obs_output_get_service(stream->output);
		if (!service)
			return false;
		path = obs_service_get_url(service);
		stream->split_file = false;
	} else {

		stream->max_time =
			obs_data_get_int(settings, "max_time_sec") * 1000000LL;
		stream->max_size = obs_data_get_int(settings, "max_size_mb") *
				   (1024 * 1024);
		stream->split_file = obs_data_get_bool(settings, "split_file");
		stream->allow_overwrite =
			obs_data_get_bool(settings, "allow_overwrite");
		stream->cur_size = 0;
		stream->sent_headers = false;
	}

	ts_offset_clear(stream);

	if (!stream->is_network) {
		/* ensure output path is writable to avoid generic error
		 * message.
		 *
		 * TODO: remove once ffmpeg-mux is refactored to pass
		 * errors back */
		FILE *test_file = os_fopen(path, "wb");
		if (!test_file) {
			set_file_not_readable_error(stream, settings, path);
			return false;
		}

		fclose(test_file);
		os_unlink(path);
	}

	start_pipe(stream, path);

	if (!stream->pipe) {
		obs_output_set_last_error(
			stream->output, obs_module_text("HelperProcessFailed"));
		warn("Failed to create process pipe");
		return false;
	}

	/* write headers and start capture */
	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	stream->total_bytes = 0;
	obs_output_begin_data_capture(stream->output, 0);

	info("Writing file '%s'...", stream->path.array);
	return true;
}

static bool ffmpeg_mux_start(void *data)
{
	struct ffmpeg_muxer *stream = data;

	obs_data_t *settings = obs_output_get_settings(stream->output);
	bool success = ffmpeg_mux_start_internal(stream, settings);
	obs_data_release(settings);

	return success;
}

int deactivate(struct ffmpeg_muxer *stream, int code)
{
	int ret = -1;

	if (stream->is_hls) {
		if (stream->mux_thread_joinable) {
			os_event_signal(stream->stop_event);
			os_sem_post(stream->write_sem);
			pthread_join(stream->mux_thread, NULL);
			stream->mux_thread_joinable = false;
		}
	}

	if (active(stream)) {
		ret = os_process_pipe_destroy(stream->pipe);
		stream->pipe = NULL;

		os_atomic_set_bool(&stream->active, false);
		os_atomic_set_bool(&stream->sent_headers, false);

		info("Output of file '%s' stopped",
		     dstr_is_empty(&stream->printable_path)
			     ? stream->path.array
			     : stream->printable_path.array);
	}

	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else if (stopping(stream)) {
		obs_output_end_data_capture(stream->output);
	}

	if (stream->is_hls) {
		pthread_mutex_lock(&stream->write_mutex);

		while (stream->packets.size) {
			struct encoder_packet packet;
			circlebuf_pop_front(&stream->packets, &packet,
					    sizeof(packet));
			obs_encoder_packet_release(&packet);
		}

		pthread_mutex_unlock(&stream->write_mutex);
	}

	os_atomic_set_bool(&stream->stopping, false);
	return ret;
}

void ffmpeg_mux_stop(void *data, uint64_t ts)
{
	struct ffmpeg_muxer *stream = data;

	if (capturing(stream) || ts == 0) {
		stream->stop_ts = (int64_t)ts / 1000LL;
		os_atomic_set_bool(&stream->stopping, true);
		os_atomic_set_bool(&stream->capturing, false);
	}
}

static void signal_failure(struct ffmpeg_muxer *stream)
{
	char error[1024];
	int ret;
	int code;

	size_t len;

	len = os_process_pipe_read_err(stream->pipe, (uint8_t *)error,
				       sizeof(error) - 1);

	if (len > 0) {
		error[len] = 0;
		warn("ffmpeg-mux: %s", error);
		obs_output_set_last_error(stream->output, error);
	}

	ret = deactivate(stream, 0);

	switch (ret) {
	case FFM_UNSUPPORTED:
		code = OBS_OUTPUT_UNSUPPORTED;
		break;
	default:
		if (stream->is_network) {
			code = OBS_OUTPUT_DISCONNECTED;
		} else {
			code = OBS_OUTPUT_ENCODE_ERROR;
		}
	}

	obs_output_signal_stop(stream->output, code);
	os_atomic_set_bool(&stream->capturing, false);
}

static void find_best_filename(struct dstr *path, bool space)
{
	int num = 2;

	if (!os_file_exists(path->array))
		return;

	const char *ext = strrchr(path->array, '.');
	if (!ext)
		return;

	size_t extstart = ext - path->array;
	struct dstr testpath;
	dstr_init_copy_dstr(&testpath, path);
	for (;;) {
		dstr_resize(&testpath, extstart);
		dstr_catf(&testpath, space ? " (%d)" : "_%d", num++);
		dstr_cat(&testpath, ext);

		if (!os_file_exists(testpath.array)) {
			dstr_free(path);
			dstr_init_move(path, &testpath);
			break;
		}
	}
}

static void generate_filename(struct ffmpeg_muxer *stream, struct dstr *dst,
			      bool overwrite)
{
	obs_data_t *settings = obs_output_get_settings(stream->output);
	const char *dir = obs_data_get_string(settings, "directory");
	const char *fmt = obs_data_get_string(settings, "format");
	const char *ext = obs_data_get_string(settings, "extension");
	bool space = obs_data_get_bool(settings, "allow_spaces");

	char *filename = os_generate_formatted_filename(ext, space, fmt);

	dstr_copy(dst, dir);
	dstr_replace(dst, "\\", "/");
	if (dstr_end(dst) != '/')
		dstr_cat_ch(dst, '/');
	dstr_cat(dst, filename);

	char *slash = strrchr(dst->array, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(dst->array);
		*slash = '/';
	}

	if (!overwrite)
		find_best_filename(dst, space);

	bfree(filename);
	obs_data_release(settings);
}

bool write_packet(struct ffmpeg_muxer *stream, struct encoder_packet *packet)
{
	bool is_video = packet->type == OBS_ENCODER_VIDEO;
	size_t ret;

	struct ffm_packet_info info = {.pts = packet->pts,
				       .dts = packet->dts,
				       .size = (uint32_t)packet->size,
				       .index = (int)packet->track_idx,
				       .type = is_video ? FFM_PACKET_VIDEO
							: FFM_PACKET_AUDIO,
				       .keyframe = packet->keyframe};

	if (stream->split_file) {
		if (is_video) {
			info.dts -= stream->video_pts_offset;
			info.pts -= stream->video_pts_offset;
		} else {
			info.dts -= stream->audio_dts_offsets[info.index];
			info.pts -= stream->audio_dts_offsets[info.index];
		}
	}

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)&info,
				    sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(stream->pipe, packet->data, packet->size);
	if (ret != packet->size) {
		warn("os_process_pipe_write for packet data failed");
		signal_failure(stream);
		return false;
	}

	stream->total_bytes += packet->size;

	if (stream->split_file)
		stream->cur_size += packet->size;

	return true;
}

static bool send_audio_headers(struct ffmpeg_muxer *stream,
			       obs_encoder_t *aencoder, size_t idx)
{
	struct encoder_packet packet = {
		.type = OBS_ENCODER_AUDIO, .timebase_den = 1, .track_idx = idx};

	if (!obs_encoder_get_extra_data(aencoder, &packet.data, &packet.size))
		return false;
	return write_packet(stream, &packet);
}

static bool send_video_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);

	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO,
					.timebase_den = 1};

	if (!obs_encoder_get_extra_data(vencoder, &packet.data, &packet.size))
		return false;
	return write_packet(stream, &packet);
}

bool send_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *aencoder;
	size_t idx = 0;

	if (!send_video_headers(stream))
		return false;

	do {
		aencoder = obs_output_get_audio_encoder(stream->output, idx);
		if (aencoder) {
			if (!send_audio_headers(stream, aencoder, idx)) {
				return false;
			}
			idx++;
		}
	} while (aencoder);

	return true;
}

static inline bool should_split(struct ffmpeg_muxer *stream,
				struct encoder_packet *packet)
{
	/* split at video frame */
	if (packet->type != OBS_ENCODER_VIDEO)
		return false;

	/* don't split group of pictures */
	if (!packet->keyframe)
		return false;

	if (os_atomic_load_bool(&stream->manual_split))
		return true;

	/* reached maximum file size */
	if (stream->max_size > 0 &&
	    stream->cur_size + (int64_t)packet->size >= stream->max_size)
		return true;

	/* reached maximum duration */
	if (stream->max_time > 0 &&
	    packet->dts_usec - stream->cur_time >= stream->max_time)
		return true;

	return false;
}

static bool send_new_filename(struct ffmpeg_muxer *stream, const char *filename)
{
	size_t ret;
	uint32_t size = (uint32_t)strlen(filename);
	struct ffm_packet_info info = {.type = FFM_PACKET_CHANGE_FILE,
				       .size = size};

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)&info,
				    sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)filename,
				    size);
	if (ret != size) {
		warn("os_process_pipe_write for packet data failed");
		signal_failure(stream);
		return false;
	}

	return true;
}

static bool prepare_split_file(struct ffmpeg_muxer *stream,
			       struct encoder_packet *packet)
{
	generate_filename(stream, &stream->path, stream->allow_overwrite);
	info("Changing output file to '%s'", stream->path.array);

	if (!send_new_filename(stream, stream->path.array)) {
		warn("Failed to send new file name");
		return false;
	}

	calldata_t cd = {0};
	signal_handler_t *sh = obs_output_get_signal_handler(stream->output);
	calldata_set_string(&cd, "next_file", stream->path.array);
	signal_handler_signal(sh, "file_changed", &cd);
	calldata_free(&cd);

	if (!send_headers(stream))
		return false;

	stream->cur_size = 0;
	stream->cur_time = packet->dts_usec;
	ts_offset_clear(stream);

	return true;
}

static inline bool has_audio(struct ffmpeg_muxer *stream)
{
	return !!obs_output_get_audio_encoder(stream->output, 0);
}

static void push_back_packet(struct darray *packets,
			     struct encoder_packet *packet)
{
	struct encoder_packet pkt;
	obs_encoder_packet_ref(&pkt, packet);
	darray_push_back(sizeof(pkt), packets, &pkt);
}

static void ffmpeg_mux_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;

	if (!active(stream))
		return;

	/* encoder failure */
	if (!packet) {
		deactivate(stream, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (stream->split_file && stream->mux_packets.num) {
		int64_t pts_usec = packet_pts_usec(packet);
		struct encoder_packet *first_pkt = stream->mux_packets.array;
		int64_t first_pts_usec = packet_pts_usec(first_pkt);

		if (pts_usec >= first_pts_usec) {
			if (packet->type != OBS_ENCODER_AUDIO) {
				push_back_packet(&stream->mux_packets.da,
						 packet);
				return;
			}

			if (!prepare_split_file(stream, first_pkt))
				return;
			stream->split_file_ready = true;
		}
	} else if (stream->split_file && should_split(stream, packet)) {
		if (has_audio(stream)) {
			push_back_packet(&stream->mux_packets.da, packet);
			return;
		} else {
			if (!prepare_split_file(stream, packet))
				return;
			stream->split_file_ready = true;
		}
	}

	if (!stream->sent_headers) {
		if (!send_headers(stream))
			return;

		stream->sent_headers = true;

		if (stream->split_file)
			stream->cur_time = packet->dts_usec;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			deactivate(stream, 0);
			return;
		}
	}

	if (stream->split_file && stream->split_file_ready) {
		for (size_t i = 0; i < stream->mux_packets.num; i++) {
			struct encoder_packet *pkt =
				&stream->mux_packets.array[i];
			ts_offset_update(stream, pkt);
			write_packet(stream, pkt);
			obs_encoder_packet_release(pkt);
		}
		da_free(stream->mux_packets);
		stream->split_file_ready = false;
		os_atomic_set_bool(&stream->manual_split, false);
	}

	if (stream->split_file)
		ts_offset_update(stream, packet);

	write_packet(stream, packet);
}

static obs_properties_t *ffmpeg_mux_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "path", obs_module_text("FilePath"),
				OBS_TEXT_DEFAULT);
	return props;
}

uint64_t ffmpeg_mux_total_bytes(void *data)
{
	struct ffmpeg_muxer *stream = data;
	return stream->total_bytes;
}

struct obs_output_info ffmpeg_muxer = {
	.id = "ffmpeg_muxer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK |
		 OBS_OUTPUT_CAN_PAUSE,
	.get_name = ffmpeg_mux_getname,
	.create = ffmpeg_mux_create,
	.destroy = ffmpeg_mux_destroy,
	.start = ffmpeg_mux_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = ffmpeg_mux_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_properties = ffmpeg_mux_properties,
};

static int connect_time(struct ffmpeg_muxer *stream)
{
	UNUSED_PARAMETER(stream);
	/* TODO */
	return 0;
}

#ifndef NEW_MPEGTS_OUTPUT
static int ffmpeg_mpegts_mux_connect_time(void *data)
{
	struct ffmpeg_muxer *stream = data;
	/* TODO */
	return connect_time(stream);
}

struct obs_output_info ffmpeg_mpegts_muxer = {
	.id = "ffmpeg_mpegts_muxer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK |
		 OBS_OUTPUT_SERVICE,
	.encoded_video_codecs = "h264;av1",
	.encoded_audio_codecs = "aac",
	.get_name = ffmpeg_mpegts_mux_getname,
	.create = ffmpeg_mux_create,
	.destroy = ffmpeg_mux_destroy,
	.start = ffmpeg_mux_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = ffmpeg_mux_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_properties = ffmpeg_mux_properties,
	.get_connect_time_ms = ffmpeg_mpegts_mux_connect_time,
};
#endif
/* ------------------------------------------------------------------------ */

static const char *replay_buffer_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("ReplayBuffer");
}

static void replay_buffer_hotkey(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_muxer *stream = data;

	if (os_atomic_load_bool(&stream->active)) {
		obs_encoder_t *vencoder =
			obs_output_get_video_encoder(stream->output);
		if (obs_encoder_paused(vencoder)) {
			info("Could not save buffer because encoders paused");
			return;
		}

		stream->save_ts = os_gettime_ns() / 1000LL;
	}
}

static void save_replay_proc(void *data, calldata_t *cd)
{
	replay_buffer_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void get_last_replay(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	if (!os_atomic_load_bool(&stream->muxing))
		calldata_set_string(cd, "path", stream->path.array);
}

static void *replay_buffer_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	stream->output = output;

	/* Check if disk buffer mode is enabled */
	stream->use_disk_buffer = is_disk_buffer_mode_enabled(output);
	
	/* Initialize disk buffer fields */
	if (stream->use_disk_buffer) {
		dstr_init(&stream->disk_buffer_dir);
		stream->current_segment_file = NULL;
		stream->current_segment_id = 0;
		stream->max_segments = 5; /* Default 5 segments */
		stream->segment_start_time = 0;
		stream->segment_duration_usec = 60 * 1000000LL; /* 60 seconds per segment */
		stream->disk_packet_count = 0;
		stream->disk_buffer_size = 0;
		info("Replay buffer initialized in segment-based disk mode");
	} else {
		info("Replay buffer initialized in RAM mode");
	}

	stream->hotkey =
		obs_hotkey_register_output(output, "ReplayBuffer.Save",
					   obs_module_text("ReplayBuffer.Save"),
					   replay_buffer_hotkey, stream);

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_add(ph, "void save()", save_replay_proc, stream);
	proc_handler_add(ph, "void get_last_replay(out string path)",
			 get_last_replay, stream);

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_add(sh, "void saved()");

	return stream;
}

static void replay_buffer_destroy(void *data)
{
	struct ffmpeg_muxer *stream = data;
	
	/* Clean up disk buffer if in disk mode */
	if (stream->use_disk_buffer) {
		cleanup_disk_buffer(stream);
	}
	
	if (stream->hotkey)
		obs_hotkey_unregister(stream->hotkey);
	ffmpeg_mux_destroy(data);
}

static bool replay_buffer_start(void *data)
{
	struct ffmpeg_muxer *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	obs_data_t *s = obs_output_get_settings(stream->output);
	stream->max_time = obs_data_get_int(s, "max_time_sec") * 1000000LL;
	stream->max_size = obs_data_get_int(s, "max_size_mb") * (1024 * 1024);

	/* Initialize DASH (m4s + MPD) when disk buffer mode enabled */
	if (stream->use_disk_buffer) {
		const char *output_dir = obs_data_get_string(s, "directory");
		if (!output_dir || !*output_dir)
			output_dir = ".";

		struct dstr dash_dir = {0};
		dstr_copy(&dash_dir, output_dir);
		dstr_cat(&dash_dir, "/dash_replay");
		os_mkdirs(dash_dir.array);

		/* manifest path */
		struct dstr mpd_path = {0};
		dstr_copy(&mpd_path, dash_dir.array);
		dstr_cat(&mpd_path, "/manifest.mpd");

		/* segment size and window - Steam과 동일하게 3초 */
		int seg_sec = 3;
		int window_size = (int)(stream->max_time / 1000000LL) / seg_sec;
		if (window_size < 2)
			window_size = 2;

		/* pass dash options to helper - Steam 방식으로 수정 */
		dstr_free(&stream->muxer_settings);
		dstr_init(&stream->muxer_settings);
		dstr_catf(&stream->muxer_settings,
			  "use_template=1 use_timeline=0 seg_duration=%d init_seg_name=init-stream$RepresentationID$.m4s media_seg_name=chunk-stream$RepresentationID$-$Number%%05d$.m4s window_size=%d extra_window_size=1 remove_at_exit=0 streaming=1",
			  seg_sec, window_size);

		start_pipe(stream, mpd_path.array);
		info("DASH output directory: %s", dash_dir.array);
		info("MPD manifest path: %s", mpd_path.array);
		
		if (!stream->pipe) {
			warn("Failed to start DASH helper process!");
		} else {
			info("DASH helper process started successfully");
		}
		dstr_free(&mpd_path);
		dstr_free(&dash_dir);
	}
	obs_data_release(s);

	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	stream->total_bytes = 0;
	stream->last_packet_time = 0;  /* 초기화 */
	obs_output_begin_data_capture(stream->output, 0);

	return true;
}

static bool purge_front(struct ffmpeg_muxer *stream)
{
	struct encoder_packet pkt;
	bool keyframe;

	if (!stream->packets.size)
		return false;

	circlebuf_pop_front(&stream->packets, &pkt, sizeof(pkt));

	keyframe = pkt.type == OBS_ENCODER_VIDEO && pkt.keyframe;

	if (keyframe)
		stream->keyframes--;

	if (!stream->packets.size) {
		stream->cur_size = 0;
		stream->cur_time = 0;
	} else {
		struct encoder_packet first;
		circlebuf_peek_front(&stream->packets, &first, sizeof(first));
		stream->cur_time = first.dts_usec;
		stream->cur_size -= (int64_t)pkt.size;
	}

	obs_encoder_packet_release(&pkt);
	return keyframe;
}

static inline void purge(struct ffmpeg_muxer *stream)
{
	if (purge_front(stream)) {
		struct encoder_packet pkt;

		for (;;) {
			if (!stream->packets.size)
				return;
			circlebuf_peek_front(&stream->packets, &pkt,
					     sizeof(pkt));
			if (pkt.type == OBS_ENCODER_VIDEO && pkt.keyframe)
				return;

			purge_front(stream);
		}
	}
}

static inline void replay_buffer_purge(struct ffmpeg_muxer *stream,
				       struct encoder_packet *pkt)
{
	if (stream->max_size) {
		if (!stream->packets.size || stream->keyframes <= 2)
			return;

		while ((stream->cur_size + (int64_t)pkt->size) >
		       stream->max_size)
			purge(stream);
	}

	if (!stream->packets.size || stream->keyframes <= 2)
		return;

	while ((pkt->dts_usec - stream->cur_time) > stream->max_time)
		purge(stream);
}

static void insert_packet(struct darray *array, struct encoder_packet *packet,
			  int64_t video_offset, int64_t *audio_offsets,
			  int64_t video_pts_offset, int64_t *audio_dts_offsets)
{
	struct encoder_packet pkt;
	DARRAY(struct encoder_packet) packets;
	packets.da = *array;
	size_t idx;

	obs_encoder_packet_ref(&pkt, packet);

	if (pkt.type == OBS_ENCODER_VIDEO) {
		pkt.dts_usec -= video_offset;
		pkt.dts -= video_pts_offset;
		pkt.pts -= video_pts_offset;
	} else {
		pkt.dts_usec -= audio_offsets[pkt.track_idx];
		pkt.dts -= audio_dts_offsets[pkt.track_idx];
		pkt.pts -= audio_dts_offsets[pkt.track_idx];
	}

	for (idx = packets.num; idx > 0; idx--) {
		struct encoder_packet *p = packets.array + (idx - 1);
		if (p->dts_usec < pkt.dts_usec)
			break;
	}

	da_insert(packets, idx, &pkt);
	*array = packets.da;
}

static void *replay_buffer_mux_thread(void *data)
{
	struct ffmpeg_muxer *stream = data;
	bool error = false;

	start_pipe(stream, stream->path.array);

	if (!stream->pipe) {
		warn("Failed to create process pipe");
		error = true;
		goto error;
	}

	if (!send_headers(stream)) {
		warn("Could not write headers for file '%s'",
		     stream->path.array);
		error = true;
		goto error;
	}

	for (size_t i = 0; i < stream->mux_packets.num; i++) {
		struct encoder_packet *pkt = &stream->mux_packets.array[i];
		write_packet(stream, pkt);
		obs_encoder_packet_release(pkt);
	}

	info("Wrote replay buffer to '%s'", stream->path.array);

error:
	os_process_pipe_destroy(stream->pipe);
	stream->pipe = NULL;
	if (error) {
		for (size_t i = 0; i < stream->mux_packets.num; i++)
			obs_encoder_packet_release(
				&stream->mux_packets.array[i]);
	}
	da_free(stream->mux_packets);
	os_atomic_set_bool(&stream->muxing, false);

	if (!error) {
		calldata_t cd = {0};
		signal_handler_t *sh =
			obs_output_get_signal_handler(stream->output);
		signal_handler_signal(sh, "saved", &cd);
	}

	return NULL;
}

static void replay_buffer_save(struct ffmpeg_muxer *stream)
{
	if (stream->use_disk_buffer) {
		/* Load packets from disk buffer for muxing */
		if (!load_packets_from_disk_for_mux(stream)) {
			warn("Failed to load packets from disk buffer for muxing");
			return;
		}
		
		/* Process disk packets the same way as RAM packets - reorder and fix timestamps */
		bool found_video = false;
		bool found_audio[MAX_AUDIO_MIXES] = {0};
		int64_t video_offset = 0;
		int64_t video_pts_offset = 0;
		int64_t audio_offsets[MAX_AUDIO_MIXES] = {0};
		int64_t audio_dts_offsets[MAX_AUDIO_MIXES] = {0};

		/* Create a copy of packets for reordering */
		DARRAY(struct encoder_packet) temp_packets;
		da_init(temp_packets);
		
		for (size_t i = 0; i < stream->mux_packets.num; i++) {
			struct encoder_packet *pkt = &stream->mux_packets.array[i];

			if (pkt->type == OBS_ENCODER_VIDEO) {
				if (!found_video) {
					video_pts_offset = pkt->pts;
					video_offset = video_pts_offset * 1000000 /
						       pkt->timebase_den;
					found_video = true;
				}
			} else {
				if (!found_audio[pkt->track_idx]) {
					found_audio[pkt->track_idx] = true;
					audio_offsets[pkt->track_idx] = pkt->dts_usec;
					audio_dts_offsets[pkt->track_idx] = pkt->dts;
				}
			}

			insert_packet(&temp_packets.da, pkt, video_offset,
				      audio_offsets, video_pts_offset,
				      audio_dts_offsets);
		}
		
		/* Replace mux_packets with reordered packets */
		for (size_t i = 0; i < stream->mux_packets.num; i++)
			obs_encoder_packet_release(&stream->mux_packets.array[i]);
		da_free(stream->mux_packets);
		stream->mux_packets.da = temp_packets.da;
		
	} else {
		/* Original RAM-based packet processing */
		const size_t size = sizeof(struct encoder_packet);
		size_t num_packets = stream->packets.size / size;

		da_reserve(stream->mux_packets, num_packets);

		/* ---------------------------- */
		/* reorder packets */

		bool found_video = false;
		bool found_audio[MAX_AUDIO_MIXES] = {0};
		int64_t video_offset = 0;
		int64_t video_pts_offset = 0;
		int64_t audio_offsets[MAX_AUDIO_MIXES] = {0};
		int64_t audio_dts_offsets[MAX_AUDIO_MIXES] = {0};

		for (size_t i = 0; i < num_packets; i++) {
			struct encoder_packet *pkt;
			pkt = circlebuf_data(&stream->packets, i * size);

			if (pkt->type == OBS_ENCODER_VIDEO) {
				if (!found_video) {
					video_pts_offset = pkt->pts;
					video_offset = video_pts_offset * 1000000 /
						       pkt->timebase_den;
					found_video = true;
				}
			} else {
				if (!found_audio[pkt->track_idx]) {
					found_audio[pkt->track_idx] = true;
					audio_offsets[pkt->track_idx] = pkt->dts_usec;
					audio_dts_offsets[pkt->track_idx] = pkt->dts;
				}
			}

			insert_packet(&stream->mux_packets.da, pkt, video_offset,
				      audio_offsets, video_pts_offset,
				      audio_dts_offsets);
		}
	}

	generate_filename(stream, &stream->path, true);

	os_atomic_set_bool(&stream->muxing, true);
	stream->mux_thread_joinable = pthread_create(&stream->mux_thread, NULL,
						     replay_buffer_mux_thread,
						     stream) == 0;
	if (!stream->mux_thread_joinable) {
		warn("Failed to create muxer thread");
		os_atomic_set_bool(&stream->muxing, false);
	}
}

static void convert_mpd_to_static(struct ffmpeg_muxer *stream)
{
	obs_data_t *s = obs_output_get_settings(stream->output);
	const char *output_dir = obs_data_get_string(s, "directory");
	if (!output_dir || !*output_dir)
		output_dir = ".";

	struct dstr mpd_path = {0};
	dstr_copy(&mpd_path, output_dir);
	dstr_cat(&mpd_path, "/obs_dash_replay/manifest.mpd");

	/* MPD 파일 읽기 */
	char *mpd_content = os_quick_read_utf8_file(mpd_path.array);
	if (!mpd_content) {
		warn("Failed to read MPD file for conversion");
		dstr_free(&mpd_path);
		obs_data_release(s);
		return;
	}

	/* Steam과 동일한 static MPD 생성 */
	struct dstr new_content = {0};
	dstr_init(&new_content);
	
	/* 실제 비디오/오디오 설정값 가져오기 */
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(stream->output, 0);
	
	/* 비디오 설정 */
	int width = obs_output_get_width(stream->output);
	int height = obs_output_get_height(stream->output);
	int video_bitrate = 8000000; /* 기본값 */
	const char *video_codec = "hev1"; /* 기본값 */
	
	if (vencoder) {
		obs_data_t *vsettings = obs_encoder_get_settings(vencoder);
		video_bitrate = (int)obs_data_get_int(vsettings, "bitrate") * 1000; /* kbps to bps */
		const char *codec_name = obs_encoder_get_codec(vencoder);
		if (codec_name) {
			if (strstr(codec_name, "h264")) video_codec = "avc1";
			else if (strstr(codec_name, "h265") || strstr(codec_name, "hevc")) video_codec = "hev1";
			else if (strstr(codec_name, "av1")) video_codec = "av01";
		}
		obs_data_release(vsettings);
	}
	
	/* 오디오 설정 */
	int audio_bitrate = 192000; /* 기본값 */
	int sample_rate = 48000; /* 기본값 */
	int channels = 2; /* 기본값 */
	
	if (aencoder) {
		obs_data_t *asettings = obs_encoder_get_settings(aencoder);
		audio_bitrate = (int)obs_data_get_int(asettings, "bitrate") * 1000; /* kbps to bps */
		sample_rate = (int)obs_encoder_get_sample_rate(aencoder);
		obs_data_release(asettings);
		
		audio_t *audio = obs_get_audio();
		if (audio) {
			channels = (int)audio_output_get_channels(audio);
		}
	}
	
	/* 프레임레이트 */
	video_t *video = obs_get_video();
	const struct video_output_info *vinfo = video_output_get_info(video);
	int fps_num = 60, fps_den = 1; /* 기본값 */
	if (vinfo) {
		fps_num = (int)vinfo->fps_num;
		fps_den = (int)vinfo->fps_den;
	}

	/* Steam과 완전히 동일한 MPD 구조로 재작성 */
	dstr_catf(&new_content, 
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<MPD xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" \n"
		"\txmlns=\"urn:mpeg:dash:schema:mpd:2011\" \n"
		"\txmlns:xlink=\"http://www.w3.org/1999/xlink\" \n"
		"\txsi:schemaLocation=\"urn:mpeg:DASH:schema:MPD:2011 http://standards.iso.org/ittf/PubliclyAvailableStandards/MPEG-DASH_schema_files/DASH-MPD.xsd\" \n"
		"\tprofiles=\"urn:mpeg:dash:profile:isoff-live:2011\"\n"
		"\ttype=\"static\"\n"
		"\tmediaPresentationDuration=\"PT%.1fS\"\n"
		"\tmaxSegmentDuration=\"PT3.0S\"\n"
		"\tminBufferTime=\"PT6.0S\">\n"
		"\t<Period id=\"0\" start=\"PT0.0S\">\n"
		"\t\t<AdaptationSet id=\"0\" contentType=\"video\" startWithSAP=\"1\" segmentAlignment=\"true\" bitstreamSwitching=\"true\" maxWidth=\"%d\" maxHeight=\"%d\">\n"
		"\t\t\t<Representation id=\"0\" mimeType=\"video/mp4\" codecs=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\">\n"
		"\t\t\t\t<SegmentTemplate timescale=\"1000000\" duration=\"3000000\" initialization=\"init-stream$RepresentationID$.m4s\" media=\"chunk-stream$RepresentationID$-$Number%%05d$.m4s\" startNumber=\"1\" />\n"
		"\t\t\t</Representation>\n"
		"\t\t</AdaptationSet>\n"
		"\t\t<AdaptationSet id=\"1\" contentType=\"audio\" startWithSAP=\"1\" segmentAlignment=\"true\" bitstreamSwitching=\"true\">\n"
		"\t\t\t<Representation id=\"1\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" bandwidth=\"%d\" audioSamplingRate=\"%d\">\n"
		"\t\t\t\t<AudioChannelConfiguration schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\"%d\" />\n"
		"\t\t\t\t<SegmentTemplate timescale=\"1000000\" duration=\"3000000\" initialization=\"init-stream$RepresentationID$.m4s\" media=\"chunk-stream$RepresentationID$-$Number%%05d$.m4s\" startNumber=\"1\" />\n"
		"\t\t\t</Representation>\n"
		"\t\t</AdaptationSet>\n"
		"\t</Period>\n"
		"</MPD>\n",
		(double)(stream->last_packet_time - stream->cur_time) / 1000000.0,
		width, height, video_codec, video_bitrate, width, height,
		audio_bitrate, sample_rate, channels);

	/* 변환된 MPD 파일 쓰기 */
	if (new_content.len > 0) {
		os_quick_write_utf8_file_safe(mpd_path.array, new_content.array, 
					      new_content.len, false, "tmp", NULL);
		info("MPD converted to static format successfully");
	}

	dstr_free(&new_content);
	dstr_free(&mpd_path);
	bfree(mpd_content);
	obs_data_release(s);
}

static void deactivate_replay_buffer(struct ffmpeg_muxer *stream, int code)
{
	/* DASH 모드: FFmpeg 프로세스 종료 후 MPD를 static으로 변환 */
	if (stream->use_disk_buffer && stream->pipe) {
		info("Converting DASH MPD to static format...");
		os_process_pipe_destroy(stream->pipe);
		stream->pipe = NULL;
		
		/* MPD 파일을 dynamic에서 static으로 변환 */
		convert_mpd_to_static(stream);
	}

	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else if (stopping(stream)) {
		obs_output_end_data_capture(stream->output);
	}

	os_atomic_set_bool(&stream->active, false);
	os_atomic_set_bool(&stream->sent_headers, false);
	os_atomic_set_bool(&stream->stopping, false);
	replay_buffer_clear(stream);
}

static void replay_buffer_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;
	struct encoder_packet pkt;

	if (!active(stream))
		return;

	/* encoder failure */
	if (!packet) {
		deactivate_replay_buffer(stream, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			deactivate_replay_buffer(stream, 0);
			return;
		}
	}

	obs_encoder_packet_ref(&pkt, packet);
	
	/* DISK MODE (DASH): continuously pipe packets to helper dash muxer */
	if (stream->use_disk_buffer) {
		info("DASH packet received: type=%d, size=%d, dts=%lld", 
		     pkt.type, pkt.size, pkt.dts_usec);
		if (!stream->sent_headers) {
			if (!send_headers(stream)) {
				warn("Failed to send headers to DASH helper");
				obs_encoder_packet_release(&pkt);
				return;
			}
			stream->sent_headers = true;
			info("DASH headers sent successfully");
		}

		/* 패킷 시간 정보 먼저 저장 */
		if (!stream->cur_time) {
			stream->cur_time = pkt.dts_usec;
			stream->last_packet_time = pkt.dts_usec;
			info("DASH first packet: cur_time=%lld", stream->cur_time);
		} else {
			/* 항상 마지막 패킷 시간 업데이트 */
			stream->last_packet_time = pkt.dts_usec;
			info("DASH packet update: last_time=%lld, duration=%.1fs", 
			     stream->last_packet_time, 
			     (double)(stream->last_packet_time - stream->cur_time) / 1000000.0);
		}
		stream->cur_size += pkt.size;

		if (!write_packet(stream, &pkt)) {
			warn("Failed to write packet to DASH helper");
		} else {
			info("DASH packet written successfully");
		}
		obs_encoder_packet_release(&pkt);
	} else {
		/* Store packet to RAM (original behavior) */
		replay_buffer_purge(stream, &pkt);

		if (!stream->packets.size)
			stream->cur_time = pkt.dts_usec;
		stream->cur_size += pkt.size;

		circlebuf_push_back(&stream->packets, packet, sizeof(*packet));
	}

	if (packet->type == OBS_ENCODER_VIDEO && packet->keyframe)
		stream->keyframes++;

	if (stream->save_ts && packet->sys_dts_usec >= stream->save_ts) {
		if (os_atomic_load_bool(&stream->muxing))
			return;

		if (stream->mux_thread_joinable) {
			pthread_join(stream->mux_thread, NULL);
			stream->mux_thread_joinable = false;
		}

		stream->save_ts = 0;
		replay_buffer_save(stream);
	}
}

static void replay_buffer_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "max_time_sec", 15);
	obs_data_set_default_int(s, "max_size_mb", 500);
	obs_data_set_default_string(s, "format", "%CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_default_string(s, "extension", "mp4");
	obs_data_set_default_bool(s, "allow_spaces", true);
	obs_data_set_default_bool(s, "use_disk_buffer", false);
}

struct obs_output_info replay_buffer = {
	.id = "replay_buffer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK |
		 OBS_OUTPUT_CAN_PAUSE,
	.get_name = replay_buffer_getname,
	.create = replay_buffer_create,
	.destroy = replay_buffer_destroy,
	.start = replay_buffer_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = replay_buffer_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_defaults = replay_buffer_defaults,
};
