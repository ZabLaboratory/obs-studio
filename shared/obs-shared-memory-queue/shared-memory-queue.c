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

struct video_queue_slot_header {
	/* Odd means the producer is writing; even means a complete publication.
	 * InterlockedExchange64 supplies release publication and the matching
	 * compare-exchange load supplies acquire observation on x86 and x64. */
	volatile uint64_t sequence;
};

struct video_queue {
	HANDLE handle;
	bool ready_to_read;
	struct queue_header *header;
	struct video_queue_frame_metadata *metadata[3];
	struct video_queue_slot_header *slot[3];
	uint64_t *timestamp[3];
	uint8_t *frame[3];
	long last_inc;
	int dup_counter;
	bool is_writer;
	bool metadata_enabled;
	bool layout_valid;
	size_t mapping_size;
	uint32_t frame_header_size;
	uint8_t *read_buffer;
	size_t read_buffer_size;
	uint32_t last_read_index;
	bool have_last_read_index;
	uint64_t next_sequence;
	struct video_queue_read_counters counters;
};

#define LEGACY_FRAME_HEADER_SIZE 32U
#define FRAME_HEADER_SIZE ((uint32_t)(sizeof(struct video_queue_slot_header) + sizeof(struct video_queue_frame_metadata)))

#if defined(__cplusplus)
static_assert(sizeof(struct video_queue_slot_header) == 8, "slot sequence ABI must be 64-bit");
#else
_Static_assert(sizeof(struct video_queue_slot_header) == 8, "slot sequence ABI must be 64-bit");
#endif

static bool queue_frame_size(uint32_t cx, uint32_t cy, size_t *frame_size)
{
	const size_t width = (size_t)cx;
	const size_t height = (size_t)cy;
	if (!width || !height || (width & 1U) != 0 || (height & 1U) != 0 || width > (size_t)-1 / height)
		return false;

	const size_t pixels = width * height;
	if (pixels > (size_t)-1 - pixels / 2)
		return false;

	*frame_size = pixels + pixels / 2;
	return true;
}

/* The DirectShow mapping is opened FILE_MAP_READ.  Interlocked* compare-
 * exchange operations are read-modify-write instructions even when the
 * value/comparand are both zero, so using them here faults on a read-only
 * view.  Aligned volatile loads are atomic for the 32/64-bit fields; the
 * acquire barrier after the load keeps the published frame payload and
 * metadata after the index/sequence observation. */
static uint32_t queue_load_index(const volatile uint32_t *value)
{
	if (!value)
		return 0;
	const uint32_t loaded = *value;
	MemoryBarrier();
	return loaded;
}

static uint64_t queue_load_sequence(const struct video_queue_slot_header *slot)
{
	if (!slot)
		return 0;
	const uint64_t loaded = slot->sequence;
	MemoryBarrier();
	return loaded;
}

static void queue_store_sequence(struct video_queue_slot_header *slot, uint64_t sequence)
{
	InterlockedExchange64((volatile LONG64 *)&slot->sequence, (LONG64)sequence);
}

static bool queue_validate_nv12(uint32_t cx, uint32_t cy, uint8_t **data, uint32_t *linesize,
				       enum video_queue_pixel_format format)
{
	return format == VIDEO_QUEUE_PIXEL_FORMAT_NV12 && data && linesize && data[0] && data[1] &&
		linesize[0] >= cx && linesize[1] >= cx && cx && cy && (cx & 1U) == 0 && (cy & 1U) == 0;
}

static void queue_copy_nv12(uint8_t *destination, uint8_t **data, const uint32_t *linesize,
				    uint32_t cx, uint32_t cy)
{
	for (uint32_t row = 0; row < cy; ++row)
		memcpy(destination + (size_t)row * cx, data[0] + (size_t)row * linesize[0], cx);

	destination += (size_t)cx * cy;
	for (uint32_t row = 0; row < cy / 2; ++row)
		memcpy(destination + (size_t)row * cx, data[1] + (size_t)row * linesize[1], cx);
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

/* A producer can publish STOPPING while a DirectShow consumer is between
 * timer ticks. Treat a view that was already unmapped (or whose section was
 * replaced) as invalid before touching queue_header atomics. */
static bool queue_mapping_live(const struct video_queue *vq)
{
	MEMORY_BASIC_INFORMATION info = {0};
	return vq && vq->layout_valid && vq->header && vq->mapping_size &&
		VirtualQuery(vq->header, &info, sizeof(info)) == sizeof(info) &&
		info.State == MEM_COMMIT && info.RegionSize != 0;
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
		vq->slot[i] = vq->metadata_enabled ?
			(struct video_queue_slot_header *)(((uint8_t *)vq->header) + off) : NULL;
		vq->metadata[i] = vq->metadata_enabled ?
			(struct video_queue_frame_metadata *)(((uint8_t *)vq->header) + off + sizeof(struct video_queue_slot_header)) : NULL;
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
	{
		size_t frame_size = 0;
		if (!queue_frame_size(vq.header->cx, vq.header->cy, &frame_size))
			goto open_failed;
		vq.read_buffer_size = frame_size;
		vq.read_buffer = malloc(frame_size);
		if (!vq.read_buffer)
			goto open_failed;
	}
	/* Legacy queues use a packed NV12 frame and modern return queues use the
	 * same payload after their sequence/metadata header. */

	struct video_queue *pvq = malloc(sizeof(vq));
	if (!pvq) {
		free(vq.read_buffer);
		UnmapViewOfFile(vq.header);
		CloseHandle(vq.handle);
		return NULL;
	}
	memcpy(pvq, &vq, sizeof(vq));
	return pvq;

open_failed:
	UnmapViewOfFile(vq.header);
	CloseHandle(vq.handle);
	free(vq.read_buffer);
	return NULL;
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
	const bool mapping_live = queue_mapping_live(vq);
	if (vq->is_writer && mapping_live) {
		InterlockedExchange((volatile LONG *)&vq->header->state, SHARED_QUEUE_STATE_STOPPING);
	}

	if (mapping_live)
		UnmapViewOfFile(vq->header);
	CloseHandle(vq->handle);
	free(vq->read_buffer);
	free(vq);
}

void video_queue_get_info(video_queue_t *vq, uint32_t *cx, uint32_t *cy, uint64_t *interval)
{
	if (!queue_mapping_live(vq))
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

bool video_queue_write_ex(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp,
				  enum video_queue_pixel_format format,
				  const struct video_queue_frame_metadata *metadata)
{
	if (!queue_mapping_live(vq) || !vq->is_writer || !queue_validate_nv12(vq->header->cx, vq->header->cy,
							 data, linesize, format))
		return false;

	struct queue_header *qh = vq->header;
	const uint32_t inc = (uint32_t)InterlockedIncrement((volatile LONG *)&qh->write_idx);

	unsigned long idx = get_idx(inc);
	if (vq->metadata_enabled) {
		if (vq->next_sequence >= (UINT64_MAX / 2U) - 1U)
			return false;
		const uint64_t publication = ++vq->next_sequence * 2U;
		queue_store_sequence(vq->slot[idx], publication - 1U);
		queue_copy_nv12(vq->frame[idx], data, linesize, qh->cx, qh->cy);
		if (metadata)
			vq->metadata[idx][0] = *metadata;
		else
			memset(vq->metadata[idx], 0, sizeof(*vq->metadata[idx]));
		vq->metadata[idx]->timestamp = timestamp;
		MemoryBarrier();
		queue_store_sequence(vq->slot[idx], publication);
	} else {
		queue_copy_nv12(vq->frame[idx], data, linesize, qh->cx, qh->cy);
		*vq->timestamp[idx] = timestamp;
	}
	InterlockedExchange((volatile LONG *)&qh->read_idx, (LONG)inc);
	InterlockedExchange((volatile LONG *)&qh->state, SHARED_QUEUE_STATE_READY);
	return true;
}

void video_queue_write(video_queue_t *vq, uint8_t **data, uint32_t *linesize, uint64_t timestamp)
{
	(void)video_queue_write_ex(vq, data, linesize, timestamp, VIDEO_QUEUE_PIXEL_FORMAT_NV12, NULL);
}

enum queue_state video_queue_state(video_queue_t *vq)
{
	if (!queue_mapping_live(vq)) {
		return SHARED_QUEUE_STATE_INVALID;
	}

	enum queue_state state = (enum queue_state)queue_load_index(&vq->header->state);
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
	if (!queue_mapping_live(vq) || !vq->ready_to_read || !scale || !dst)
		return false;

	struct queue_header *qh = vq->header;
	const uint32_t inc = queue_load_index(&qh->read_idx);

	if (queue_load_index(&qh->state) == SHARED_QUEUE_STATE_STOPPING) {
		return false;
	}

	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		const uint32_t candidate = attempt == 0 ? inc : queue_load_index(&qh->read_idx);
		const unsigned long idx = get_idx(candidate);
		struct video_queue_frame_metadata metadata_copy = {0};
		const uint64_t before = vq->metadata_enabled ? queue_load_sequence(vq->slot[idx]) : 0;
		if (vq->metadata_enabled && (before == 0 || (before & 1U) != 0)) {
			++vq->counters.retry_count;
			continue;
		}

		if (vq->metadata_enabled)
			metadata_copy = vq->metadata[idx][0];
		else if (!vq->read_buffer || vq->read_buffer_size < (size_t)vq->header->cx * vq->header->cy * 3 / 2)
			return false;
		memcpy(vq->read_buffer, vq->frame[idx], vq->read_buffer_size);
		const uint64_t after = vq->metadata_enabled ? queue_load_sequence(vq->slot[idx]) : before;
		if (vq->metadata_enabled && (before != after || (after & 1U) != 0)) {
			++vq->counters.retry_count;
			++vq->counters.torn_count;
			continue;
		}

		if (vq->have_last_read_index) {
			const uint32_t delta = candidate - vq->last_read_index;
			if (delta == 0) {
				++vq->counters.duplicate_count;
				if (++vq->dup_counter == 10)
					return false;
			} else {
				if (delta > 1)
					vq->counters.gap_count += delta - 1U;
				vq->dup_counter = 0;
			}
		}
		vq->last_read_index = candidate;
		vq->have_last_read_index = true;
		if (metadata) {
			if (vq->metadata_enabled)
				*metadata = metadata_copy;
		}
		if (ts)
			*ts = vq->metadata_enabled ? metadata_copy.timestamp : *vq->timestamp[idx];
		nv12_do_scale(scale, dst, vq->read_buffer);
		return true;
	}
	return false;
}

void video_queue_get_read_counters(video_queue_t *vq, struct video_queue_read_counters *counters)
{
	if (counters)
		*counters = vq ? vq->counters : (struct video_queue_read_counters){0};
}

bool video_queue_read(video_queue_t *vq, nv12_scale_t *scale, void *dst, uint64_t *ts)
{
	return video_queue_read_ex(vq, scale, dst, ts, NULL);
}
