#include <windows.h>
#include "shared-memory-queue.h"
#include "tiny-nv12-scale.h"

#include <stddef.h>

#define VIDEO_NAME L"OBSVirtualCamVideo"

enum queue_type {
	SHARED_QUEUE_TYPE_VIDEO,
};

struct queue_header {
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t state;

	uint32_t offsets[3];

	uint32_t type;

	uint32_t cx;
	uint32_t cy;
	uint64_t interval;

	/* Zero for the legacy queue layout; FRAME_HEADER_SIZE plus
	 * VIDEO_QUEUE_METADATA_VERSION for queues that carry the #246 runtime
	 * correlation envelope.  These two fields occupy the first two legacy
	 * reserved words, so the 80-byte header ABI is unchanged. */
	uint32_t frame_header_size;
	uint32_t frame_metadata_version;
	uint32_t reserved[6];
};

struct video_queue {
	HANDLE handle;
	bool ready_to_read;
	struct queue_header *header;
	struct video_queue_frame_metadata *metadata[3];
	uint64_t *timestamp[3];
	uint8_t *frame[3];
	long last_inc;
	int dup_counter;
	bool is_writer;
	bool metadata_enabled;
	bool layout_valid;
	size_t mapping_size;
	uint32_t frame_header_size;
};

#define LEGACY_FRAME_HEADER_SIZE 32U
#define FRAME_HEADER_SIZE ((uint32_t)sizeof(struct video_queue_frame_metadata))

static bool queue_frame_size(uint32_t cx, uint32_t cy, size_t *frame_size)
{
	const size_t width = (size_t)cx;
	const size_t height = (size_t)cy;
	if (!width || !height || width > (size_t)-1 / height)
		return false;

	const size_t pixels = width * height;
	if (pixels > (size_t)-1 - pixels / 2)
		return false;

	*frame_size = pixels + pixels / 2;
	return true;
}

static bool queue_add_dword(DWORD *size, size_t addition)
{
	if (!size || addition > (size_t)UINT32_MAX - (size_t)*size)
		return false;
	*size += (DWORD)addition;
	return true;
}

static bool queue_align_dword(DWORD *size)
{
	if (!size || *size > UINT32_MAX - 31U)
		return false;
	*size = (*size + 31U) & ~31U;
	return true;
}

static bool queue_metadata_layout(const struct queue_header *header, uint32_t *frame_header_size,
					 bool *metadata_enabled)
{
	if (!header || !frame_header_size || !metadata_enabled)
		return false;

	if (header->frame_header_size == 0 && header->frame_metadata_version == 0) {
		*frame_header_size = LEGACY_FRAME_HEADER_SIZE;
		*metadata_enabled = false;
		return true;
	}

	if (header->frame_header_size == FRAME_HEADER_SIZE &&
		header->frame_metadata_version == VIDEO_QUEUE_METADATA_VERSION) {
		*frame_header_size = FRAME_HEADER_SIZE;
		*metadata_enabled = true;
		return true;
	}

	/* Never reinterpret a partially populated/unknown extension as legacy. */
	return false;
}

static bool queue_mapping_size(const void *mapping, size_t *mapping_size)
{
	MEMORY_BASIC_INFORMATION info = {0};
	if (!mapping || !mapping_size || VirtualQuery(mapping, &info, sizeof(info)) != sizeof(info) ||
		info.BaseAddress != mapping || info.State != MEM_COMMIT || info.RegionSize < sizeof(struct queue_header))
		return false;

	*mapping_size = (size_t)info.RegionSize;
	return true;
}

static bool queue_layout_valid(const struct queue_header *header, size_t mapping_size,
				       uint32_t frame_header_size)
{
	size_t frame_size;
	if (!header || mapping_size < sizeof(*header) || header->type != SHARED_QUEUE_TYPE_VIDEO ||
		!queue_frame_size(header->cx, header->cy, &frame_size))
		return false;

	uint32_t previous_offset = 0;
	for (size_t i = 0; i < 3; ++i) {
		const uint32_t offset = header->offsets[i];
		if (offset < sizeof(*header) || (offset & 31U) != 0 || (i && offset <= previous_offset))
			return false;

		size_t cursor = (size_t)offset;
		if (cursor > mapping_size || (size_t)frame_header_size > mapping_size - cursor)
			return false;
		cursor += frame_header_size;
		if (frame_size > mapping_size - cursor)
			return false;
		previous_offset = offset;
	}

	return true;
}

static bool queue_initialize_layout(struct video_queue *vq, size_t mapping_size)
{
	uint32_t frame_header_size = 0;
	bool metadata_enabled = false;
	if (!vq || !queue_metadata_layout(vq->header, &frame_header_size, &metadata_enabled) ||
		!queue_layout_valid(vq->header, mapping_size, frame_header_size))
		return false;

	vq->mapping_size = mapping_size;
	vq->frame_header_size = frame_header_size;
	vq->metadata_enabled = metadata_enabled;
	vq->layout_valid = true;
	return true;
}

static void queue_set_frame_pointers(struct video_queue *vq)
{
	for (size_t i = 0; i < 3; ++i) {
		const uint32_t off = vq->header->offsets[i];
		vq->metadata[i] = vq->metadata_enabled ?
			(struct video_queue_frame_metadata *)(((uint8_t *)vq->header) + off) : NULL;
		vq->timestamp[i] = vq->metadata_enabled ? &vq->metadata[i]->timestamp :
			(uint64_t *)(((uint8_t *)vq->header) + off);
		vq->frame[i] = ((uint8_t *)vq->header) + off + vq->frame_header_size;
	}
	vq->ready_to_read = true;
}

static bool queue_uses_metadata(const wchar_t *name)
{
	/* Keep the public OBSVirtualCamVideo ABI byte-for-byte compatible.  The
	 * named ProgramReturn/PreviewReturn queues are created by the patched
	 * producer and are the only queues consumed by the telemetry-aware
	 * DirectShow filter. */
	return name && (wcsstr(name, L"ProgramReturnVideo") ||
			wcsstr(name, L"PreviewReturnVideo"));
}

video_queue_t *video_queue_create_named(uint32_t cx, uint32_t cy, uint64_t interval,
									 const wchar_t *name)
{
	struct video_queue vq = {0};
	struct video_queue *pvq;
	size_t frame_size;
	const bool metadata_enabled = queue_uses_metadata(name);
	const uint32_t frame_header_size = metadata_enabled ? FRAME_HEADER_SIZE : LEGACY_FRAME_HEADER_SIZE;
	uint32_t offset_frame[3];
	DWORD size;

	if (!queue_frame_size(cx, cy, &frame_size) || frame_size > UINT32_MAX)
		return NULL;

	size = sizeof(struct queue_header);

	if (!queue_align_dword(&size))
		return NULL;

	offset_frame[0] = size;
	if (!queue_add_dword(&size, frame_size) || !queue_add_dword(&size, frame_header_size) ||
		!queue_align_dword(&size))
		return NULL;

	offset_frame[1] = size;
	if (!queue_add_dword(&size, frame_size) || !queue_add_dword(&size, frame_header_size) ||
		!queue_align_dword(&size))
		return NULL;

	offset_frame[2] = size;
	if (!queue_add_dword(&size, frame_size) || !queue_add_dword(&size, frame_header_size) ||
		!queue_align_dword(&size))
		return NULL;

	struct queue_header header = {0};

	header.state = SHARED_QUEUE_STATE_STARTING;
	header.type = SHARED_QUEUE_TYPE_VIDEO;
	header.cx = cx;
	header.cy = cy;
	header.interval = interval;
	header.frame_header_size = metadata_enabled ? FRAME_HEADER_SIZE : 0;
	header.frame_metadata_version = metadata_enabled ? VIDEO_QUEUE_METADATA_VERSION : 0;
	vq.is_writer = true;

	for (size_t i = 0; i < 3; i++) {
		uint32_t off = offset_frame[i];
		header.offsets[i] = off;
	}

	/* fail if already in use */
	vq.handle = OpenFileMappingW(FILE_MAP_READ, false, name);
	if (vq.handle) {
		CloseHandle(vq.handle);
		return NULL;
	}

	vq.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, name);
	if (!vq.handle) {
		return NULL;
	}

	vq.header = (struct queue_header *)MapViewOfFile(vq.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!vq.header) {
		CloseHandle(vq.handle);
		return NULL;
	}
	memcpy(vq.header, &header, sizeof(header));
	if (!queue_initialize_layout(&vq, (size_t)size)) {
		UnmapViewOfFile(vq.header);
		CloseHandle(vq.handle);
		return NULL;
	}

	queue_set_frame_pointers(&vq);
	pvq = malloc(sizeof(vq));
	if (!pvq) {
		UnmapViewOfFile(vq.header);
		CloseHandle(vq.handle);
		return NULL;
	}
	memcpy(pvq, &vq, sizeof(vq));
	return pvq;
}

video_queue_t *video_queue_open_named(const wchar_t *name)
{
	struct video_queue vq = {0};

	vq.handle = OpenFileMappingW(FILE_MAP_READ, false, name);
	if (!vq.handle) {
		return NULL;
	}

	vq.header = (struct queue_header *)MapViewOfFile(vq.handle, FILE_MAP_READ, 0, 0, 0);
	if (!vq.header) {
		CloseHandle(vq.handle);
		return NULL;
	}

	if (!queue_mapping_size(vq.header, &vq.mapping_size) ||
		!queue_initialize_layout(&vq, vq.mapping_size)) {
		UnmapViewOfFile(vq.header);
		CloseHandle(vq.handle);
		return NULL;
	}

	struct video_queue *pvq = malloc(sizeof(vq));
	if (!pvq) {
		UnmapViewOfFile(vq.header);
		CloseHandle(vq.handle);
		return NULL;
	}
	memcpy(pvq, &vq, sizeof(vq));
	return pvq;
}

video_queue_t *video_queue_create(uint32_t cx, uint32_t cy, uint64_t interval)
{
	return video_queue_create_named(cx, cy, interval, VIDEO_NAME);
}

video_queue_t *video_queue_open()
{
	return video_queue_open_named(VIDEO_NAME);
}

void video_queue_close(video_queue_t *vq)
{
	if (!vq) {
		return;
	}
	if (vq->is_writer) {
		vq->header->state = SHARED_QUEUE_STATE_STOPPING;
	}

	UnmapViewOfFile(vq->header);
	CloseHandle(vq->handle);
	free(vq);
}

void video_queue_get_info(video_queue_t *vq, uint32_t *cx, uint32_t *cy, uint64_t *interval)
{
	if (!vq || !vq->layout_valid)
		return;

	struct queue_header *qh = vq->header;
	if (cx)
		*cx = qh->cx;
	if (cy)
		*cy = qh->cy;
	if (interval)
		*interval = qh->interval;
}

#define get_idx(inc) ((unsigned long)inc % 3)

void video_queue_write_ex(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp,
				  const struct video_queue_frame_metadata *metadata)
{
	if (!vq || !vq->layout_valid || !vq->is_writer || !data || !linesize)
		return;

	struct queue_header *qh = vq->header;
	long inc = ++qh->write_idx;

	unsigned long idx = get_idx(inc);
	size_t size = linesize[0] * qh->cy;

	memcpy(vq->frame[idx], data[0], size);
	memcpy(vq->frame[idx] + size, data[1], size / 2);
	if (vq->metadata_enabled) {
		if (metadata)
			vq->metadata[idx][0] = *metadata;
		else
			memset(vq->metadata[idx], 0, sizeof(*vq->metadata[idx]));
		vq->metadata[idx]->timestamp = timestamp;
	} else {
		/* The original OBSVirtualCamVideo queue stores its timestamp as the
		 * first eight bytes before each NV12 frame. */
		*vq->timestamp[idx] = timestamp;
	}

	qh->read_idx = inc;
	qh->state = SHARED_QUEUE_STATE_READY;
}

void video_queue_write(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp)
{
	video_queue_write_ex(vq, data, linesize, timestamp, NULL);
}

enum queue_state video_queue_state(video_queue_t *vq)
{
	if (!vq || !vq->layout_valid) {
		return SHARED_QUEUE_STATE_INVALID;
	}

	enum queue_state state = (enum queue_state)vq->header->state;
	if (!vq->ready_to_read && state == SHARED_QUEUE_STATE_READY) {
		queue_set_frame_pointers(vq);
	}

	return state;
}

bool video_queue_read_ex(video_queue_t *vq, nv12_scale_t *scale, void *dst, uint64_t *ts,
				 struct video_queue_frame_metadata *metadata)
{
	if (metadata)
		memset(metadata, 0, sizeof(*metadata));
	if (!vq || !vq->layout_valid || !vq->ready_to_read || !scale || !dst)
		return false;

	struct queue_header *qh = vq->header;
	long inc = qh->read_idx;

	if (qh->state == SHARED_QUEUE_STATE_STOPPING) {
		return false;
	}

	if (inc == vq->last_inc) {
		if (++vq->dup_counter == 10) {
			return false;
		}
	} else {
		vq->dup_counter = 0;
		vq->last_inc = inc;
	}

	unsigned long idx = get_idx(inc);

	if (metadata) {
		if (vq->metadata_enabled)
			*metadata = vq->metadata[idx][0];
	}
	if (ts)
		*ts = *vq->timestamp[idx];

	nv12_do_scale(scale, dst, vq->frame[idx]);
	return true;
}

bool video_queue_read(video_queue_t *vq, nv12_scale_t *scale, void *dst, uint64_t *ts)
{
	return video_queue_read_ex(vq, scale, dst, ts, NULL);
}
