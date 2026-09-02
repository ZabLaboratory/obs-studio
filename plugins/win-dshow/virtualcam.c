#include <obs-module.h>
#include <util/platform.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include "util/threading.h"
#include "../../shared/obs-shared-memory-queue/shared-memory-queue.h"
#include "directshow-namespace.h"
#include "virtualcam-module/d3d11-return-transport.hpp"
#include "../../../plugins/pulsar-frontend-stub/include/pulsar-runtime-telemetry-abi.h"

#define PULSAR_DIRECTSHOW_LEASE_POLL_MS 20U

enum return_lease_state {
	RETURN_LEASE_STOPPED = 0,
	RETURN_LEASE_STARTING,
	RETURN_LEASE_DETACHED,
	RETURN_LEASE_ATTACHED,
	RETURN_LEASE_RECONNECTING,
};

struct virtualcam_data {
	obs_output_t *output;
	video_queue_t *vq;
	wchar_t queue_name[256];
	wchar_t consumer_lease_name[288];
	bool queue_namespace_rejected;
	bool program_return;
	bool preview_return;
	bool consumer_gated;
	volatile long consumer_active;
	volatile long lease_state;
	volatile long lease_watcher_stop;
	volatile long lease_watcher_started;
	volatile long lease_watcher_fallback;
	volatile long lease_polls;
	volatile long lease_hits;
	volatile long lease_misses;
	volatile long lease_expiry;
	bool lease_telemetry_enabled;
	os_event_t *lease_wakeup;
	pthread_t lease_thread;
	pulsar_d3d11_return_producer_t *d3d11;
	bool d3d11_requested;
	volatile bool active;
	volatile bool stopping;
};

static const char *return_lease_role(const struct virtualcam_data *vcam)
{
	return vcam->program_return ? "ProgramReturn" : "PreviewReturn";
}

static bool return_lease_telemetry_enabled(void)
{
	const char *value = getenv("PULSAR_DIRECTSHOW_LEASE_TELEMETRY");
	return value && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0);
}

static const char *return_lease_state_name(enum return_lease_state state)
{
	switch (state) {
	case RETURN_LEASE_STARTING:
		return "start";
	case RETURN_LEASE_DETACHED:
		return "detach";
	case RETURN_LEASE_ATTACHED:
		return "attach";
	case RETURN_LEASE_RECONNECTING:
		return "reconnect";
	case RETURN_LEASE_STOPPED:
	default:
		return "stop";
	}
}

static void return_lease_log_state(struct virtualcam_data *vcam, enum return_lease_state state)
{
	const long previous = os_atomic_exchange_long(&vcam->lease_state, (long)state);
	if (previous != (long)state)
		blog(LOG_INFO, "[pulsar-directshow] %s lease state=%s name=%ls", return_lease_role(vcam),
		     return_lease_state_name(state), vcam->consumer_lease_name);
}

static bool return_lease_probe_once(struct virtualcam_data *vcam)
{
	InterlockedIncrement(&vcam->lease_polls);
	HANDLE lease = OpenEventW(SYNCHRONIZE, FALSE, vcam->consumer_lease_name);
	const bool active = lease != NULL;
	const DWORD probe_error = active ? ERROR_SUCCESS : GetLastError();
	if (lease)
		CloseHandle(lease);

	if (active) {
		InterlockedIncrement(&vcam->lease_hits);
		InterlockedExchange(&vcam->consumer_active, 1L);
		const long state = os_atomic_load_long(&vcam->lease_state);
		return_lease_log_state(vcam,
				       state == RETURN_LEASE_DETACHED ? RETURN_LEASE_RECONNECTING
									 : RETURN_LEASE_ATTACHED);
	} else {
		InterlockedIncrement(&vcam->lease_misses);
		if (probe_error != ERROR_FILE_NOT_FOUND && probe_error != ERROR_PATH_NOT_FOUND)
			InterlockedIncrement(&vcam->lease_watcher_fallback);
		const long previous = InterlockedExchange(&vcam->consumer_active, 0L);
		if (previous)
			InterlockedIncrement(&vcam->lease_expiry);
		return_lease_log_state(vcam, RETURN_LEASE_DETACHED);
	}

	return active;
}

static void *return_lease_watcher(void *param)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)param;

	while (!os_atomic_load_long(&vcam->lease_watcher_stop)) {
		return_lease_probe_once(vcam);
		const int wait_result = os_event_timedwait(vcam->lease_wakeup, PULSAR_DIRECTSHOW_LEASE_POLL_MS);
		if (wait_result != ETIMEDOUT) {
			if (wait_result != 0)
				InterlockedIncrement(&vcam->lease_watcher_fallback);
			break;
		}
	}

	os_atomic_set_long(&vcam->consumer_active, 0L);
	return_lease_log_state(vcam, RETURN_LEASE_STOPPED);
	return NULL;
}

static bool return_lease_watcher_start(struct virtualcam_data *vcam)
{
	if (!vcam->consumer_gated) {
		InterlockedIncrement(&vcam->lease_watcher_fallback);
		return true;
	}

	if (os_atomic_load_long(&vcam->lease_watcher_started))
		return true;

	os_atomic_set_long(&vcam->consumer_active, 0L);
	os_atomic_set_long(&vcam->lease_watcher_stop, 0L);
	return_lease_log_state(vcam, RETURN_LEASE_STARTING);
	vcam->lease_wakeup = NULL;
	if (os_event_init(&vcam->lease_wakeup, OS_EVENT_TYPE_MANUAL) != 0 ||
	    pthread_create(&vcam->lease_thread, NULL, return_lease_watcher, vcam) != 0) {
		if (vcam->lease_wakeup) {
			os_event_destroy(vcam->lease_wakeup);
			vcam->lease_wakeup = NULL;
		}
		InterlockedIncrement(&vcam->lease_watcher_fallback);
		os_atomic_set_long(&vcam->consumer_active, 0L);
		return_lease_log_state(vcam, RETURN_LEASE_DETACHED);
		return false;
	}
	os_atomic_set_long(&vcam->lease_watcher_started, 1L);
	return true;
}

static void return_lease_log_counters(const struct virtualcam_data *vcam)
{
	if (vcam->lease_telemetry_enabled)
		blog(LOG_INFO,
		     "[pulsar-directshow] %s lease telemetry polls=%ld hits=%ld misses=%ld expiry=%ld fallback=%ld poll_ms=%u",
		     return_lease_role(vcam), os_atomic_load_long(&vcam->lease_polls),
		     os_atomic_load_long(&vcam->lease_hits), os_atomic_load_long(&vcam->lease_misses),
		     os_atomic_load_long(&vcam->lease_expiry), os_atomic_load_long(&vcam->lease_watcher_fallback),
		     PULSAR_DIRECTSHOW_LEASE_POLL_MS);
}

static void return_lease_watcher_stop(struct virtualcam_data *vcam)
{
	if (!vcam->consumer_gated) {
		return_lease_log_counters(vcam);
		return;
	}

	os_atomic_set_long(&vcam->consumer_active, 0L);
	if (!os_atomic_load_long(&vcam->lease_watcher_started)) {
		return_lease_log_counters(vcam);
		return_lease_log_state(vcam, RETURN_LEASE_STOPPED);
		return;
	}

	os_atomic_set_long(&vcam->lease_watcher_stop, 1L);
	if (vcam->lease_wakeup)
		os_event_signal(vcam->lease_wakeup);
	pthread_join(vcam->lease_thread, NULL);
	os_atomic_set_long(&vcam->lease_watcher_started, 0L);
	if (vcam->lease_wakeup) {
		os_event_destroy(vcam->lease_wakeup);
		vcam->lease_wakeup = NULL;
	}
	return_lease_log_counters(vcam);
	return_lease_log_state(vcam, RETURN_LEASE_STOPPED);
}

static bool return_consumer_is_active(struct virtualcam_data *vcam)
{
	if (!vcam->consumer_gated)
		return true;

	return os_atomic_load_long(&vcam->consumer_active) != 0;
}

static void copy_telemetry_identifier(char *destination, const char *value)
{
	if (!value)
		value = "";
	strncpy(destination, value, PULSAR_RUNTIME_TELEMETRY_IDENTIFIER_CAPACITY - 1);
	destination[PULSAR_RUNTIME_TELEMETRY_IDENTIFIER_CAPACITY - 1] = '\0';
}

static bool copy_telemetry_counter(uint64_t *destination, long long value)
{
	if (!destination || value < 0 || value > INT64_MAX)
		return false;
	*destination = (uint64_t)value;
	return true;
}

static void snapshot_runtime_frame(struct video_queue_frame_metadata *metadata)
{
	memset(metadata, 0, sizeof(*metadata));
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	calldata_t cd = {0};
	if (!proc_handler_call(ph, PULSAR_RUNTIME_TELEMETRY_SNAPSHOT_PROC, &cd)) {
		calldata_free(&cd);
		return;
	}

	const bool valid = calldata_bool(&cd, "valid");
	uint64_t server_seq = 0;
	uint64_t frame_id = 0;
	uint64_t pts_ns = 0;
	uint64_t program_revision = 0;
	uint64_t preview_revision = 0;
	uint64_t role_map_revision = 0;
	const bool counters_valid =
		copy_telemetry_counter(&server_seq, calldata_int(&cd, "server_seq")) &&
		copy_telemetry_counter(&frame_id, calldata_int(&cd, "frame_id")) &&
		copy_telemetry_counter(&pts_ns, calldata_int(&cd, "pts_ns")) &&
		copy_telemetry_counter(&program_revision, calldata_int(&cd, "program_revision")) &&
		copy_telemetry_counter(&preview_revision, calldata_int(&cd, "preview_revision")) &&
		copy_telemetry_counter(&role_map_revision, calldata_int(&cd, "role_map_revision"));
	if (!valid || !counters_valid) {
		calldata_free(&cd);
		return;
	}

	metadata->server_seq = server_seq;
	metadata->frame_id = frame_id;
	metadata->pts_ns = pts_ns;
	metadata->program_revision = program_revision;
	metadata->preview_revision = preview_revision;
	metadata->role_map_revision = role_map_revision;
	metadata->valid = 1U;
	copy_telemetry_identifier(metadata->runtime_instance_id, calldata_string(&cd, "runtime_instance_id"));
	copy_telemetry_identifier(metadata->command_id, calldata_string(&cd, "command_id"));
	copy_telemetry_identifier(metadata->intent_id, calldata_string(&cd, "intent_id"));
	copy_telemetry_identifier(metadata->take_command_id, calldata_string(&cd, "take_command_id"));
	calldata_free(&cd);
}

static enum video_queue_pixel_format queue_pixel_format(obs_output_t *output)
{
	const struct video_scale_info *conversion = obs_output_get_video_conversion(output);
	/* Some obs_output implementations expose the conversion only after the
	 * first data-capture callback.  The return outputs are created with the
	 * canonical NV12 conversion, so keep the producer live during that short
	 * initialization window instead of dropping every frame as INVALID. */
	if (!conversion)
		return VIDEO_QUEUE_PIXEL_FORMAT_NV12;
	if (conversion->format == VIDEO_FORMAT_NV12)
		return VIDEO_QUEUE_PIXEL_FORMAT_NV12;
	if (conversion->format == VIDEO_FORMAT_P010)
		return VIDEO_QUEUE_PIXEL_FORMAT_P010;
	return VIDEO_QUEUE_PIXEL_FORMAT_INVALID;
}

static bool queue_name_for_output(obs_output_t *output, wchar_t *destination, size_t capacity)
{
	const char *output_name = obs_output_get_name(output);
	const char *output_id = obs_output_get_id(output);
	const bool has_output_id = output_id && *output_id;
	const bool program_return = has_output_id ? strcmp(output_id, "program_return_output") == 0
							 : output_name && strcmp(output_name, "PulsarProgramReturn") == 0;
	const bool preview_return = has_output_id ? strcmp(output_id, "preview_return_output") == 0
							 : output_name && strcmp(output_name, "PulsarPreviewReturn") == 0;
	const wchar_t *legacy_name = preview_return ? L"OBSPulsarPreviewReturnVideo"
						   : program_return ? L"OBSPulsarProgramReturnVideo" : L"OBSVirtualCamVideo";
	const enum directshow_queue_namespace queue_namespace = directshow_queue_namespace_from_environment();

	if (queue_namespace == DIRECTSHOW_QUEUE_NAMESPACE_REJECT)
		return false;
	if (queue_namespace == DIRECTSHOW_QUEUE_NAMESPACE_LEGACY) {
		wcsncpy(destination, legacy_name, capacity - 1);
		destination[capacity - 1] = 0;
		return true;
	}
	const char *runtime_id = getenv("PULSAR_RUNTIME_INSTANCE_ID");

	wchar_t wide_id[65] = {0};
	/* IDs are validated as ASCII above; avoid a libobs link from this DLL. */
	for (size_t i = 0; runtime_id[i] != '\0'; ++i)
		wide_id[i] = (wchar_t)(unsigned char)runtime_id[i];

	_snwprintf_s(destination, capacity, _TRUNCATE, L"Local\\Pulsar.%ls.%ls", wide_id,
			     preview_return ? L"PreviewReturnVideo"
					     : program_return ? L"ProgramReturnVideo" : L"VirtualCamVideo");
	return true;
}

static const char *virtualcam_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Virtual Camera Output";
}

static void virtualcam_destroy(void *data)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	return_lease_watcher_stop(vcam);
	pulsar_d3d11_return_producer_close(vcam->d3d11);
	video_queue_close(vcam->vq);
	bfree(data);
}

static void *virtualcam_create(obs_data_t *settings, obs_output_t *output)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)bzalloc(sizeof(*vcam));
	vcam->output = output;
	vcam->program_return = obs_output_get_id(output) &&
				       strcmp(obs_output_get_id(output), "program_return_output") == 0;
	vcam->preview_return = obs_output_get_id(output) &&
				       strcmp(obs_output_get_id(output), "preview_return_output") == 0;
	vcam->consumer_gated = vcam->program_return || vcam->preview_return;
	vcam->lease_telemetry_enabled = return_lease_telemetry_enabled();
	const char *transport = getenv("PULSAR_RETURN_TRANSPORT");
	vcam->d3d11_requested = vcam->consumer_gated && transport && strcmp(transport, "d3d11") == 0;
	vcam->queue_namespace_rejected = !queue_name_for_output(
		output, vcam->queue_name, sizeof(vcam->queue_name) / sizeof(vcam->queue_name[0]));
	if (vcam->consumer_gated && !vcam->queue_namespace_rejected)
		_snwprintf_s(vcam->consumer_lease_name,
			     sizeof(vcam->consumer_lease_name) / sizeof(vcam->consumer_lease_name[0]), _TRUNCATE,
			     L"%ls.ConsumerActive", vcam->queue_name);
	if (vcam->queue_namespace_rejected)
		blog(LOG_ERROR, "[pulsar-directshow] queue namespace rejected; producer is disabled");

	UNUSED_PARAMETER(settings);
	return vcam;
}

static bool virtualcam_start(void *data)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	if (vcam->queue_namespace_rejected)
		return false;
	uint32_t width = obs_output_get_width(vcam->output);
	uint32_t height = obs_output_get_height(vcam->output);

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	uint64_t interval = ovi.fps_den * 10000000ULL / ovi.fps_num;

	char res[64];
	snprintf(res, sizeof(res), "%dx%dx%lld", (int)width, (int)height, (long long)interval);

	if (wcscmp(vcam->queue_name, L"OBSVirtualCamVideo") == 0) {
		char *res_file = os_get_config_path_ptr("obs-virtualcam.txt");
		os_quick_write_utf8_file_safe(res_file, res, strlen(res), false, "tmp", NULL);
		bfree(res_file);
	}

	vcam->vq = video_queue_create_named(width, height, interval, vcam->queue_name);
	if (!vcam->vq) {
		const char *runtime_id = getenv("PULSAR_RUNTIME_INSTANCE_ID");
		blog(LOG_WARNING, "starting virtual-output failed (queue=%ls runtime_instance_id=%s)",
		     vcam->queue_name, runtime_id ? runtime_id : "");
		return false;
	}

	struct video_scale_info vsi = {0};
	vsi.format = VIDEO_FORMAT_NV12;
	vsi.width = width;
	vsi.height = height;
	obs_output_set_video_conversion(vcam->output, &vsi);
	if (vcam->d3d11_requested) {
		const enum pulsar_d3d11_return_lane lane = vcam->program_return ? PULSAR_D3D11_PROGRAM_RETURN
										 : PULSAR_D3D11_PREVIEW_RETURN;
		vcam->d3d11 = pulsar_d3d11_return_producer_create(vcam->queue_name, lane, width, height);
		if (!vcam->d3d11)
			blog(LOG_WARNING, "[pulsar-directshow] D3D11 %s control unavailable; using CPU return queue",
			     vcam->program_return ? "ProgramReturn" : "PreviewReturn");
	}
	if (!return_lease_watcher_start(vcam))
		blog(LOG_WARNING, "[pulsar-directshow] %s lease watcher unavailable; publication remains disabled",
		     return_lease_role(vcam));

	os_atomic_set_bool(&vcam->active, true);
	os_atomic_set_bool(&vcam->stopping, false);
	blog(LOG_INFO, "Virtual output started (queue=%ls namespace=%s)", vcam->queue_name,
	     directshow_queue_namespace_name(directshow_queue_namespace_from_environment()));
	obs_output_begin_data_capture(vcam->output, 0);
	return true;
}

static void virtualcam_deactive(struct virtualcam_data *vcam)
{
	os_atomic_set_bool(&vcam->active, false);
	obs_output_end_data_capture(vcam->output);
	pulsar_d3d11_return_producer_close(vcam->d3d11);
	vcam->d3d11 = NULL;
	video_queue_close(vcam->vq);
	vcam->vq = NULL;

	os_atomic_set_bool(&vcam->stopping, false);

	blog(LOG_INFO, "Virtual output stopped");
}

static void virtualcam_stop(void *data, uint64_t ts)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	os_atomic_set_bool(&vcam->stopping, true);
	return_lease_watcher_stop(vcam);

	blog(LOG_INFO, "Virtual output stopping");

	UNUSED_PARAMETER(ts);
}

static void virtual_video(void *param, struct video_data *frame)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)param;

	if (!vcam->vq)
		return;

	if (!os_atomic_load_bool(&vcam->active))
		return;

	if (os_atomic_load_bool(&vcam->stopping)) {
		virtualcam_deactive(vcam);
		return;
	}

	/* Both compositions stay hot and Program encoding remains independent.
	 * Only the optional DirectShow shared-memory publication is elided while
	 * no graph actively consumes this return. */
	if (!return_consumer_is_active(vcam))
		return;

	struct video_queue_frame_metadata metadata;
	snapshot_runtime_frame(&metadata);
	const enum video_queue_pixel_format format = queue_pixel_format(vcam->output);
	if (vcam->d3d11 && format == VIDEO_QUEUE_PIXEL_FORMAT_NV12 &&
	    pulsar_d3d11_return_producer_write(vcam->d3d11, frame->data, frame->linesize, frame->timestamp, &metadata))
		return;
	(void)video_queue_write_ex(vcam->vq, frame->data, frame->linesize, frame->timestamp, format, &metadata);
}

struct obs_output_info virtualcam_info = {
	.id = "virtualcam_output",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = virtualcam_name,
	.create = virtualcam_create,
	.destroy = virtualcam_destroy,
	.start = virtualcam_start,
	.stop = virtualcam_stop,
	.raw_video = virtual_video,
};

struct obs_output_info program_return_info = {
	.id = "program_return_output",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = virtualcam_name,
	.create = virtualcam_create,
	.destroy = virtualcam_destroy,
	.start = virtualcam_start,
	.stop = virtualcam_stop,
	.raw_video = virtual_video,
	.raw_video_borrowed = virtual_video,
};

struct obs_output_info preview_return_info = {
	.id = "preview_return_output",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = virtualcam_name,
	.create = virtualcam_create,
	.destroy = virtualcam_destroy,
	.start = virtualcam_start,
	.stop = virtualcam_stop,
	.raw_video = virtual_video,
	.raw_video_borrowed = virtual_video,
};
