#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

struct video_queue;
struct nv12_scale;
typedef struct video_queue video_queue_t;
typedef struct nv12_scale nv12_scale_t;

/* Frame metadata travels with the NV12 slot, not in a process-local side
 * channel.  That is what lets an out-of-process DirectShow reader report the
 * exact ProgramReturn frame it consumed.  The first field intentionally keeps
 * the legacy queue timestamp layout; old callers can continue using
 * video_queue_read()/video_queue_write(). */
#define VIDEO_QUEUE_IDENTIFIER_CAPACITY 129
#define VIDEO_QUEUE_METADATA_VERSION 1U
struct video_queue_frame_metadata {
	uint64_t timestamp;
	uint64_t frame_id;
	uint64_t pts_ns;
	uint64_t server_seq;
	uint64_t program_revision;
	uint64_t preview_revision;
	uint64_t role_map_revision;
	uint32_t valid;
	char runtime_instance_id[VIDEO_QUEUE_IDENTIFIER_CAPACITY];
	char command_id[VIDEO_QUEUE_IDENTIFIER_CAPACITY];
	char intent_id[VIDEO_QUEUE_IDENTIFIER_CAPACITY];
	char take_command_id[VIDEO_QUEUE_IDENTIFIER_CAPACITY];
};

enum queue_state {
	SHARED_QUEUE_STATE_INVALID,
	SHARED_QUEUE_STATE_STARTING,
	SHARED_QUEUE_STATE_READY,
	SHARED_QUEUE_STATE_STOPPING,
};

extern video_queue_t *video_queue_create(uint32_t cx, uint32_t cy, uint64_t interval);
extern video_queue_t *video_queue_open();
extern video_queue_t *video_queue_create_named(uint32_t cx, uint32_t cy, uint64_t interval,
									 const wchar_t *name);
extern video_queue_t *video_queue_open_named(const wchar_t *name);
extern void video_queue_close(video_queue_t *vq);

extern void video_queue_get_info(video_queue_t *vq, uint32_t *cx, uint32_t *cy, uint64_t *interval);
extern void video_queue_write(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp);
extern void video_queue_write_ex(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp,
					 const struct video_queue_frame_metadata *metadata);
extern enum queue_state video_queue_state(video_queue_t *vq);
extern bool video_queue_read(video_queue_t *vq, nv12_scale_t *scale, void *dst, uint64_t *ts);
extern bool video_queue_read_ex(video_queue_t *vq, nv12_scale_t *scale, void *dst, uint64_t *ts,
					struct video_queue_frame_metadata *metadata);

#ifdef __cplusplus
}
#endif
