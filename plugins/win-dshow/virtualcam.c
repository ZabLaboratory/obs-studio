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
#include "../../../plugins/pulsar-frontend-stub/include/pulsar-runtime-telemetry-abi.h"

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
	volatile bool active;
	volatile bool stopping;
};

static bool return_consumer_is_active(struct virtualcam_data *vcam)
{
	if (!vcam->consumer_gated)
		return true;

	HANDLE lease = OpenEventW(SYNCHRONIZE, FALSE, vcam->consumer_lease_name);
	const bool active = lease != NULL;
	if (lease)
		CloseHandle(lease);

	const long previous = InterlockedExchange(&vcam->consumer_active, active ? 1L : 0L);
	if (previous != (active ? 1L : 0L))
		blog(LOG_INFO, "[pulsar-directshow] %s consumer %s (lease=%ls)",
		     vcam->program_return ? "ProgramReturn" : "PreviewReturn", active ? "attached" : "detached",
		     vcam->consumer_lease_name);
	return active;
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

	os_atomic_set_bool(&vcam->active, true);
	os_atomic_set_bool(&vcam->stopping, false);
	blog(LOG_INFO, "Virtual output started (queue=%ls namespace=%s)", vcam->queue_name,
	     directshow_queue_namespace_name(directshow_queue_namespace_from_environment()));
	obs_output_begin_data_capture(vcam->output, 0);
	return true;
}

static void virtualcam_deactive(struct virtualcam_data *vcam)
{
	obs_output_end_data_capture(vcam->output);
	video_queue_close(vcam->vq);
	vcam->vq = NULL;

	os_atomic_set_bool(&vcam->active, false);
	os_atomic_set_bool(&vcam->stopping, false);

	blog(LOG_INFO, "Virtual output stopped");
}

static void virtualcam_stop(void *data, uint64_t ts)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
	os_atomic_set_bool(&vcam->stopping, true);

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
	(void)video_queue_write_ex(vcam->vq, frame->data, frame->linesize, frame->timestamp,
				   queue_pixel_format(vcam->output), &metadata);
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
