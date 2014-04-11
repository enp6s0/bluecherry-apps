#include <unistd.h>
#include "recorder.h"
#include "bc-server.h"
#include "media_writer.h"

static void event_trigger_notifications(bc_event_cam_t event);

recorder::recorder(const bc_record *bc_rec)
	: stream_consumer("Recorder"), device_id(bc_rec->id), destroy_flag(false),
	  recording_type(BC_EVENT_CAM_T_CONTINUOUS), writer(0), current_event(0)
{
}

recorder::~recorder()
{
	delete writer;
}

void recorder::set_recording_type(bc_event_cam_type_t type)
{
	recording_type = type;
}

void recorder::destroy()
{
	destroy_flag = true;
	buffer_wait.notify_all();
}

void recorder::run()
{
	std::shared_ptr<const stream_properties> saved_properties;

	bc_log_context_push(log);

	std::unique_lock<std::mutex> l(lock);
	while (!destroy_flag)
	{
		if (buffer.empty()) {
			buffer_wait.wait(l);
			continue;
		}

		stream_packet packet = buffer.front();
		buffer.pop_front();
		l.unlock();

		if (!packet.data()) {
			/* Null packets force recording end */
			recording_end();
			l.lock();
			continue;
		}

		if (packet.properties() != saved_properties) {
			bc_log(Debug, "recorder: stream properties changed");
			saved_properties = packet.properties();
			recording_end();
		}

		if (current_event && packet.is_key_frame() && packet.is_video_frame() &&
				bc_event_media_length(current_event) > BC_MAX_RECORD_TIME) {
			recording_end();
		}

		/* Setup and write to recordings */
		if (!writer) {
			if (!packet.is_key_frame() || !packet.is_video_frame()) {
				/* Recordings must always start with a video keyframe; this should
				 * be ensured in most cases (continuous splits, initialization), but
				 * if absolutely necessary we'll drop packets. */
				goto end;
			}

			if (recording_start(packet.ts_clock, packet)) {
				bc_log(Error, "Cannot start recording!");
				recording_end();
				sleep(10);
				goto end;
			}
		}

		writer->write_packet(packet);

		/* Snapshot(s) producing */

		/* TODO Pass decoded picture to stream_packet if available. This is not
		 * always the case (e.g. V4L2 devices pass encoded packets and we don't
		 * decode it for motion detection because it is done by device itself, also
		 * we don't decode if we just pull recording from e.g. ONVIF camera that
		 * has on-camera motion detection.
		 */

		if (recording_type == BC_EVENT_CAM_T_MOTION) {
			if (snapshotting_proceeding) {
				int ret = writer->snapshot_feed(packet);
				if (ret < 0) {
					bc_log(Error, "Failed while feeding snapshot saver with more frames");
					snapshotting_proceeding = false;
				} else if (ret > 0) {
					bc_log(Debug, "Still need to feed more frames to finish snapshot");
				} else {
					bc_log(Debug, "Finalized snapshot");
					snapshotting_proceeding = false;
					snapshots_done++;
					// FIXME Seems it doesn't work with custom www paths and launches by hardcoded path:
					// Could not open input file: /usr/share/bluecherry/www/lib/mailer.php
					event_trigger_notifications(current_event);
				}
			} else if (snapshots_done < snapshots_limit
					&& packet.is_video_frame() && packet.is_key_frame()
					&& packet.ts_monotonic > (first_packet_ts_monotonic + buffer.duration()
						/* TODO higher precision for time storage */
						/* TODO support millisecond precision for delay option */
						+ (snapshotting_delay_since_motion_start_ms/1000))) {
				bc_log(Debug, "Making a snapshot");

				// Push frames to decoder until picture is taken
				// In some cases one AVPacket marked as keyframe is not enough, and next
				// packet must be pushed to decoder, too.
				int ret = writer->snapshot_create(snapshot_filename.c_str(), packet);
				if (ret < 0) {
					bc_log(Error, "Failed to make snapshot");
				} else if (ret > 0) {
					bc_log(Debug, "Need to feed more frames to finish snapshot");
					snapshotting_proceeding = true;
				} else {
					bc_log(Debug, "Saved snapshot from single keyframe");
				}
			}
		}

end:
		l.lock();
	}

	bc_log(Debug, "recorder destroying");
	l.unlock();
	recording_end();
	bc_event_cam_end(&current_event);
	delete this;
}

static void event_trigger_notifications(bc_event_cam_t event)
{
	pid_t pid = fork();
	if (pid < 0) {
		bc_log(Bug, "cannot fork for event notification");
		return;
	}

	/* Parent process */
	if (pid)
		return;

	char id[24] = { 0 };
	snprintf(id, sizeof(id), "%lu", event->media.table_id);
	execl("/usr/bin/php", "/usr/bin/php", "/usr/share/bluecherry/www/lib/mailer.php", id, NULL);
	exit(1);
}


/**
 * Get the path for a media file on the current device with the given start
 * time, creating folders if necessary. Repeated calls may return different
 * results; use writer to get the current media file's path.
 */
static
char *media_file_path(char *dst, size_t len, time_t start_ts, int device_id)
{
	struct tm tm;
	char date[12], fname[14];

	size_t loc_len = bc_get_media_loc(dst, len);
	if (loc_len > len)
		return NULL;

	localtime_r(&start_ts, &tm);
	strftime(date, sizeof(date), "%Y/%m/%d", &tm);
	strftime(fname, sizeof(fname), "/%H-%M-%S.mkv", &tm);

	len -= loc_len;
	size_t dir_len = snprintf(dst + loc_len, len, "/%s/%06d",
				  date, device_id);
	if (dir_len >= len)
		return NULL;

	if (bc_mkdir_recursive(dst) < 0) {
		bc_log(Error, "Cannot create media directory %s: %m", dst);
		return NULL;
	}

	len -= dir_len;
	dst += loc_len + dir_len;

	/* Append file name */
	memcpy(dst, fname, len);

	/* Return pointer to file extension */
	return dst + 10;
}

int recorder::recording_start(time_t start_ts, const stream_packet &first_packet)
{
	bc_event_cam_t nevent = NULL;

	if (!start_ts)
		start_ts = time(NULL);

	recording_end();

	snapshotting_proceeding = false;
	snapshots_limit = 1;  /* TODO Optionize */
	snapshots_done = 0;
	snapshotting_delay_since_motion_start_ms = snapshot_delay_ms;

	char outfile[PATH_MAX];
	char *ext = media_file_path(outfile, sizeof(outfile), start_ts,
				    device_id);
	if (!ext) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	writer = new media_writer;
	if (writer->open(std::string(outfile), *first_packet.properties().get()) != 0) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	bc_event_level_t level = (recording_type == BC_EVENT_CAM_T_MOTION) ? BC_EVENT_L_WARN : BC_EVENT_L_INFO;
	nevent = bc_event_cam_start(device_id, start_ts, level, recording_type, outfile);

	if (!nevent) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	bc_event_cam_end(&current_event);
	current_event = nevent;

	// Save timestamp of first packet
	first_packet_ts_monotonic = first_packet.ts_monotonic;
	strcpy(ext, "jpg");
	snapshot_filename = outfile;

	return 0;
}

void recorder::recording_end()
{
	/* Close the media entry in the db */
	if (current_event && bc_event_has_media(current_event))
		bc_event_cam_end(&current_event);

	if (writer)
		writer->close();
	delete writer;
	writer = 0;
}

void recorder::do_error_event(bc_event_level_t level, bc_event_cam_type_t type)
{
	if (!current_event || current_event->level != level || current_event->type != type) {
		recording_end();
		bc_event_cam_end(&current_event);

		current_event = bc_event_cam_start(device_id, time(NULL), level, type, NULL);
	}
}

void recorder::set_buffer_time(int prerecord)
{
	buffer.set_duration(prerecord);
}

