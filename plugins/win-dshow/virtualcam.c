#include <obs-module.h>
#include <util/platform.h>
#include <string.h>
#include "util/threading.h"
#include "shared-memory-queue.h"

struct virtualcam_data {
	obs_output_t *output;
	video_queue_t *vq;
	const wchar_t *queue_name;
	volatile bool active;
	volatile bool stopping;
};

static const wchar_t *queue_name_for_output(obs_output_t *output)
{
	const char *name = obs_output_get_name(output);
	return name && strcmp(name, "PulsarProgramReturn") == 0 ? L"OBSPulsarProgramReturnVideo"
											 : L"OBSVirtualCamVideo";
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
	vcam->queue_name = queue_name_for_output(output);

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

	if (vcam->queue_name[3] == L'V') {
		char *res_file = os_get_config_path_ptr("obs-virtualcam.txt");
		os_quick_write_utf8_file_safe(res_file, res, strlen(res), false, "tmp", NULL);
		bfree(res_file);
	}

	vcam->vq = video_queue_create_named(width, height, interval, vcam->queue_name);
	if (!vcam->vq) {
		blog(LOG_WARNING, "starting virtual-output failed");
		return false;
	}

	struct video_scale_info vsi = {0};
	vsi.format = VIDEO_FORMAT_NV12;
	vsi.width = width;
	vsi.height = height;
	obs_output_set_video_conversion(vcam->output, &vsi);

	os_atomic_set_bool(&vcam->active, true);
	os_atomic_set_bool(&vcam->stopping, false);
	blog(LOG_INFO, "Virtual output started");
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
