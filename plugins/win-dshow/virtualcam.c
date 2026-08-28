#include <obs-module.h>
#include <util/platform.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "util/threading.h"
#include "shared-memory-queue.h"

struct virtualcam_data {
	obs_output_t *output;
	video_queue_t *vq;
	wchar_t queue_name[256];
	volatile bool active;
	volatile bool stopping;
};

static bool directshow_legacy_alias_enabled(void)
{
	const char *value = getenv("PULSAR_DIRECTSHOW_LEGACY_ALIAS");
	if (!value || !*value)
		return true; /* stock OBS keeps its historical virtual-camera names */
	return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
	       strcmp(value, "on") == 0;
}

static bool valid_runtime_instance_id(const char *value)
{
	if (!value || !*value || strlen(value) > 64)
		return false;
	if (!((*value >= 'A' && *value <= 'Z') || (*value >= 'a' && *value <= 'z') ||
	      (*value >= '0' && *value <= '9')))
		return false;
	for (const char *p = value; *p; ++p) {
		if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		       (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
			return false;
	}
	return strcmp(value, ".") != 0 && strcmp(value, "..") != 0;
}

static void queue_name_for_output(obs_output_t *output, wchar_t *destination, size_t capacity)
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
	const char *runtime_id = getenv("PULSAR_RUNTIME_INSTANCE_ID");

	if (directshow_legacy_alias_enabled() || !valid_runtime_instance_id(runtime_id)) {
		wcsncpy(destination, legacy_name, capacity - 1);
		destination[capacity - 1] = 0;
		return;
	}

	wchar_t wide_id[65] = {0};
	/* IDs are validated as ASCII above; avoid a libobs link from this DLL. */
	for (size_t i = 0; runtime_id[i] != '\0'; ++i)
		wide_id[i] = (wchar_t)(unsigned char)runtime_id[i];

	_snwprintf_s(destination, capacity, _TRUNCATE, L"Local\\Pulsar.%ls.%ls", wide_id,
			     preview_return ? L"PreviewReturnVideo"
					     : program_return ? L"ProgramReturnVideo" : L"VirtualCamVideo");
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
	queue_name_for_output(output, vcam->queue_name, sizeof(vcam->queue_name) / sizeof(vcam->queue_name[0]));

	UNUSED_PARAMETER(settings);
	return vcam;
}

static bool virtualcam_start(void *data)
{
	struct virtualcam_data *vcam = (struct virtualcam_data *)data;
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
	{
		const char *runtime_id = getenv("PULSAR_RUNTIME_INSTANCE_ID");
		blog(LOG_INFO, "Virtual output started (queue=%ls runtime_instance_id=%s legacy_alias=%s)",
		     vcam->queue_name, runtime_id ? runtime_id : "",
		     directshow_legacy_alias_enabled() ? "1" : "0");
	}
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

	video_queue_write(vcam->vq, frame->data, frame->linesize, frame->timestamp);
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
};
