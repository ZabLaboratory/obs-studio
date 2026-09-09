/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include "obs.h"
#include "obs-internal.h"

bool obs_view_init(struct obs_view *view, enum view_type type)
{
	if (!view)
		return false;

	pthread_mutex_init_value(&view->channels_mutex);

	if (pthread_mutex_init(&view->channels_mutex, NULL) != 0) {
		blog(LOG_ERROR, "obs_view_init: Failed to create mutex");
		return false;
	}

	view->type = type;
	return true;
}

static obs_view_t *obs_view_create_with_type(enum view_type type)
{
	struct obs_view *view = bzalloc(sizeof(struct obs_view));

	if (!obs_view_init(view, type)) {
		bfree(view);
		view = NULL;
	}

	return view;
}

obs_view_t *obs_view_create(void)
{
	return obs_view_create_with_type(AUX_VIEW);
}

obs_view_t *obs_view_create_active(void)
{
	return obs_view_create_with_type(MAIN_VIEW);
}

void obs_view_free(struct obs_view *view)
{
	if (!view)
		return;

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source = view->channels[i];
		if (source) {
			obs_source_deactivate(source, view->type);
			obs_source_release(source);
		}
	}

	memset(view->channels, 0, sizeof(view->channels));
	pthread_mutex_destroy(&view->channels_mutex);
}

void obs_view_destroy(obs_view_t *view)
{
	if (view) {
		obs_view_free(view);
		bfree(view);
	}
}

obs_source_t *obs_view_get_source(obs_view_t *view, uint32_t channel)
{
	obs_source_t *source;
	assert(channel < MAX_CHANNELS);

	if (!view)
		return NULL;
	if (channel >= MAX_CHANNELS)
		return NULL;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(view->channels[channel]);
	pthread_mutex_unlock(&view->channels_mutex);

	return source;
}

void obs_view_set_source(obs_view_t *view, uint32_t channel, obs_source_t *source)
{
	struct obs_source *prev_source;

	assert(channel < MAX_CHANNELS);

	if (!view)
		return;
	if (channel >= MAX_CHANNELS)
		return;

	pthread_mutex_lock(&view->channels_mutex);
	source = obs_source_get_ref(source);
	prev_source = view->channels[channel];
	view->channels[channel] = source;

	pthread_mutex_unlock(&view->channels_mutex);

	if (source)
		obs_source_activate(source, view->type);

	if (prev_source) {
		obs_source_deactivate(prev_source, view->type);
		obs_source_release(prev_source);
	}
}

static void release_atomic_swap(struct obs_view_atomic_swap *swap)
{
	if (!swap)
		return;

	if (swap->first_source)
		obs_source_release(swap->first_source);
	if (swap->second_source)
		obs_source_release(swap->second_source);
	bfree(swap);
}

static bool atomic_swap_is_graphics_thread(void)
{
	if (!obs->video.thread_initialized)
		return false;

	return pthread_equal(pthread_self(), obs->video.video_thread) != 0;
}

bool obs_view_queue_atomic_swap_with_floor(obs_view_t *first_view, uint32_t first_channel,
					obs_source_t *first_source, obs_view_t *second_view,
					uint32_t second_channel, obs_source_t *second_source,
					uint64_t admission_floor_ns, obs_view_atomic_swap_cb callback, void *param)
{
	if (!obs || !obs->video.atomic_swap_initialized || !first_view || !second_view || first_view == second_view ||
	    first_channel >= MAX_CHANNELS || second_channel >= MAX_CHANNELS)
		return false;

	struct obs_view_atomic_swap *swap = bzalloc(sizeof(*swap));
	swap->first_view = first_view;
	swap->first_channel = first_channel;
	swap->second_view = second_view;
	swap->second_channel = second_channel;
	swap->first_source = obs_source_get_ref(first_source);
	swap->second_source = obs_source_get_ref(second_source);
	swap->callback = callback;
	swap->callback_param = param;
	swap->admission_floor_ns = admission_floor_ns;

	/* A non-null input must have produced a retained reference. */
	if ((first_source && !swap->first_source) || (second_source && !swap->second_source)) {
		release_atomic_swap(swap);
		return false;
	}

	pthread_mutex_lock(&obs->video.atomic_swap_mutex);
	/* The pending pointer is the admission guard.  Once the graphics thread
	 * has detached it, a successor may queue while the previous callback is
	 * still unwinding; atomic_swap_inflight remains solely a teardown drain
	 * barrier. */
	if (obs->video.pending_atomic_swap) {
		pthread_mutex_unlock(&obs->video.atomic_swap_mutex);
		release_atomic_swap(swap);
		return false;
	}
	obs->video.pending_atomic_swap = swap;
	pthread_mutex_unlock(&obs->video.atomic_swap_mutex);
	return true;
}

bool obs_view_queue_atomic_swap(obs_view_t *first_view, uint32_t first_channel,
					obs_source_t *first_source, obs_view_t *second_view,
					uint32_t second_channel, obs_source_t *second_source,
					obs_view_atomic_swap_cb callback, void *param)
{
	return obs_view_queue_atomic_swap_with_floor(first_view, first_channel, first_source,
									second_view, second_channel, second_source, 0, callback, param);
}

void obs_view_cancel_atomic_swap(void)
{
	if (!obs || !obs->video.atomic_swap_initialized)
		return;

	pthread_mutex_lock(&obs->video.atomic_swap_mutex);
	struct obs_view_atomic_swap *swap = obs->video.pending_atomic_swap;
	obs->video.pending_atomic_swap = NULL;
	/* A caller may race the graphics thread after it has taken the pending
	 * request.  Keep the mutex while waiting so teardown cannot proceed until
	 * the callback has returned and no code can still dereference its views or
	 * callback parameter.  The graphics thread itself must not wait here: a
	 * callback is allowed to cancel its own request. */
	if (obs->video.atomic_swap_inflight && !atomic_swap_is_graphics_thread()) {
		while (obs->video.atomic_swap_inflight)
			pthread_cond_wait(&obs->video.atomic_swap_cond, &obs->video.atomic_swap_mutex);
	}
	pthread_mutex_unlock(&obs->video.atomic_swap_mutex);

	release_atomic_swap(swap);
}

void obs_view_apply_pending_atomic_swap(uint64_t frame_id, uint64_t pts_ns)
{
	if (!obs || !obs->video.atomic_swap_initialized)
		return;

	pthread_mutex_lock(&obs->video.atomic_swap_mutex);
	struct obs_view_atomic_swap *swap = obs->video.pending_atomic_swap;
	/* Keep the exact pending request until the graphics timestamp reaches the
	 * immutable admission floor.  No refs, callback, or inflight state may be
	 * touched by an earlier frame. */
	if (swap && pts_ns < swap->admission_floor_ns) {
		pthread_mutex_unlock(&obs->video.atomic_swap_mutex);
		return;
	}
	obs->video.pending_atomic_swap = NULL;
	if (swap)
		obs->video.atomic_swap_inflight = true;
	pthread_mutex_unlock(&obs->video.atomic_swap_mutex);

	if (!swap)
		return;

	/* Lock both channel arrays in a stable order.  The graphics thread is the
	 * only renderer, so once both locks are held no output can observe a
	 * half-applied pair. */
	const bool first_before_second = (uintptr_t)swap->first_view < (uintptr_t)swap->second_view;
	if (first_before_second) {
		pthread_mutex_lock(&swap->first_view->channels_mutex);
		pthread_mutex_lock(&swap->second_view->channels_mutex);
	} else {
		pthread_mutex_lock(&swap->second_view->channels_mutex);
		pthread_mutex_lock(&swap->first_view->channels_mutex);
	}

	struct obs_source *old_first = swap->first_view->channels[swap->first_channel];
	struct obs_source *old_second = swap->second_view->channels[swap->second_channel];
	struct obs_source *new_first = swap->first_source;
	struct obs_source *new_second = swap->second_source;
	/* A dual-lane cut exchanges two already-visible roots: ProgramView is the
	 * MAIN_VIEW and PreviewView is AUX_VIEW.  The visibility ownership remains
	 * one view per source, so skip show_refs churn and transfer only the MAIN
	 * activation ownership.  Generic swaps, including Fade/Stinger, retain the
	 * original activate/deactivate path below. */
	const bool preserve_active_pair =
		old_first && old_second && new_first && new_second && old_first != old_second &&
		new_first == old_second && new_second == old_first &&
		swap->first_view->type == MAIN_VIEW && swap->second_view->type == AUX_VIEW;
	/* The references acquired by queue_atomic_swap now belong to the views. */
	swap->first_source = NULL;
	swap->second_source = NULL;
	swap->first_view->channels[swap->first_channel] = new_first;
	swap->second_view->channels[swap->second_channel] = new_second;

	pthread_mutex_unlock(&swap->first_view->channels_mutex);
	pthread_mutex_unlock(&swap->second_view->channels_mutex);

	if (preserve_active_pair) {
		obs_source_transfer_main_activation(old_first, new_first);
	} else {
		if (new_first)
			obs_source_activate(new_first, swap->first_view->type);
		if (new_second)
			obs_source_activate(new_second, swap->second_view->type);
	}
	if (old_first) {
		if (!preserve_active_pair)
			obs_source_deactivate(old_first, swap->first_view->type);
		obs_source_release(old_first);
	}
	if (old_second) {
		if (!preserve_active_pair)
			obs_source_deactivate(old_second, swap->second_view->type);
		obs_source_release(old_second);
	}

	if (swap->callback)
		swap->callback(swap->callback_param, frame_id, pts_ns);

	/* Publish completion only after the callback returns.  This is the drain
	 * barrier used by teardown before it destroys either view. */
	pthread_mutex_lock(&obs->video.atomic_swap_mutex);
	obs->video.atomic_swap_inflight = false;
	pthread_cond_broadcast(&obs->video.atomic_swap_cond);
	pthread_mutex_unlock(&obs->video.atomic_swap_mutex);

	release_atomic_swap(swap);
}

void obs_view_render(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&view->channels_mutex);

	for (size_t i = 0; i < MAX_CHANNELS; i++) {
		struct obs_source *source;

		source = view->channels[i];

		if (source) {
			if (source->removed) {
				obs_source_release(source);
				view->channels[i] = NULL;
			} else {
				obs_source_video_render(source);
			}
		}
	}

	pthread_mutex_unlock(&view->channels_mutex);
}

video_t *obs_view_add(obs_view_t *view)
{
	if (!obs->data.main_canvas->mix)
		return NULL;
	return obs_view_add2(view, &obs->data.main_canvas->mix->ovi);
}

video_t *obs_view_add2(obs_view_t *view, struct obs_video_info *ovi)
{
	return obs_view_add3(view, ovi, 6);
}

video_t *obs_view_add3(obs_view_t *view, struct obs_video_info *ovi, size_t cache_size)
{
	if (!view || !ovi || cache_size == 0)
		return NULL;

	struct obs_core_video_mix *mix = obs_create_video_mix_with_cache(ovi, cache_size);
	if (!mix) {
		return NULL;
	}
	mix->view = view;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &mix);
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	return mix->video;
}

void obs_view_remove(obs_view_t *view)
{
	if (!view)
		return;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		if (obs->video.mixes.array[i]->view == view)
			obs->video.mixes.array[i]->view = NULL;
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

void obs_view_enum_video_info(obs_view_t *view, bool (*enum_proc)(void *, struct obs_video_info *), void *param)
{
	pthread_mutex_lock(&obs->video.mixes_mutex);

	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->view != view)
			continue;
		if (!enum_proc(param, &mix->ovi))
			break;
	}

	pthread_mutex_unlock(&obs->video.mixes_mutex);
}
