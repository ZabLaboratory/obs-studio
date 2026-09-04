#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSAR_D3D11_RETURN_SLOT_COUNT 3U
#define PULSAR_D3D11_RETURN_ABI_VERSION 2U
#define PULSAR_D3D11_RETURN_WIDTH 1920U
#define PULSAR_D3D11_RETURN_HEIGHT 1080U
#define PULSAR_D3D11_RETURN_IDENTIFIER_CAPACITY 129U

enum pulsar_d3d11_return_lane {
	PULSAR_D3D11_PROGRAM_RETURN = 1,
	PULSAR_D3D11_PREVIEW_RETURN = 2,
};

enum pulsar_d3d11_return_path {
	PULSAR_D3D11_PATH_CPU = 0,
	PULSAR_D3D11_PATH_SHARED_TEXTURE = 1,
};

enum pulsar_d3d11_return_fallback {
	PULSAR_D3D11_FALLBACK_NONE = 0,
	PULSAR_D3D11_FALLBACK_NOT_REQUESTED = 1,
	PULSAR_D3D11_FALLBACK_CAPABILITY = 2,
	PULSAR_D3D11_FALLBACK_ADAPTER = 3,
	PULSAR_D3D11_FALLBACK_DEVICE_REMOVED = 4,
	PULSAR_D3D11_FALLBACK_TIMEOUT = 5,
	PULSAR_D3D11_FALLBACK_FORMAT = 6,
	PULSAR_D3D11_FALLBACK_INTEROP = 7,
};

struct pulsar_d3d11_return_luid {
	uint32_t low;
	int32_t high;
};

struct pulsar_d3d11_return_handle {
	uint64_t value;
};

struct pulsar_d3d11_return_slot {
	struct pulsar_d3d11_return_handle handle;
	uint64_t sequence;
	uint64_t epoch;
	uint64_t timestamp;
	uint64_t frame_id;
	uint64_t pts_ns;
	uint64_t server_seq;
	uint64_t program_revision;
	uint64_t preview_revision;
	uint64_t role_map_revision;
	uint32_t valid;
	uint32_t reserved;
	char runtime_instance_id[PULSAR_D3D11_RETURN_IDENTIFIER_CAPACITY];
	char command_id[PULSAR_D3D11_RETURN_IDENTIFIER_CAPACITY];
	char intent_id[PULSAR_D3D11_RETURN_IDENTIFIER_CAPACITY];
	char take_command_id[PULSAR_D3D11_RETURN_IDENTIFIER_CAPACITY];
};

struct pulsar_d3d11_return_control {
	uint32_t abi_version;
	uint32_t lane;
	uint32_t width;
	uint32_t height;
	uint32_t producer_pid;
	uint32_t consumer_pid;
	uint32_t producer_session;
	uint32_t consumer_session;
	uint32_t consumer_ready;
	uint32_t selected_path;
	uint32_t fallback_reason;
	int32_t fallback_hresult;
	struct pulsar_d3d11_return_luid adapter_luid;
	uint64_t epoch;
	uint64_t produced_sequence;
	uint64_t published_sequence;
	uint64_t consumed_sequence;
	uint64_t mutex_wait_ns;
	uint64_t fence_wait_ns;
	uint64_t gpu_copy_ns;
	uint64_t readback_ns;
	uint64_t gap_count;
	uint64_t retry_count;
	uint64_t torn_count;
	uint64_t frame_age_ns;
	struct pulsar_d3d11_return_slot slots[PULSAR_D3D11_RETURN_SLOT_COUNT];
};

#ifdef __cplusplus
static_assert(sizeof(struct pulsar_d3d11_return_handle) == 8, "shared handle ABI must be uint64");
static_assert(sizeof(struct pulsar_d3d11_return_slot) == 608, "slot ABI must be fixed-width");
static_assert(sizeof(struct pulsar_d3d11_return_luid) == 8, "adapter LUID ABI must be fixed-width");
static_assert(sizeof(struct pulsar_d3d11_return_control) == 1976, "control ABI must be fixed-width");
#endif

typedef struct pulsar_d3d11_return_producer pulsar_d3d11_return_producer_t;
typedef struct pulsar_d3d11_return_consumer pulsar_d3d11_return_consumer_t;
struct video_queue_frame_metadata;

pulsar_d3d11_return_producer_t *pulsar_d3d11_return_producer_create(
	const wchar_t *control_name, enum pulsar_d3d11_return_lane lane, uint32_t width, uint32_t height);
void pulsar_d3d11_return_producer_close(pulsar_d3d11_return_producer_t *producer);
bool pulsar_d3d11_return_producer_set_consumer_pid(pulsar_d3d11_return_producer_t *producer, uint32_t pid);
bool pulsar_d3d11_return_producer_ready(pulsar_d3d11_return_producer_t *producer);
bool pulsar_d3d11_return_producer_write(pulsar_d3d11_return_producer_t *producer, uint8_t **data,
						uint32_t *linesize, uint64_t timestamp,
						const struct video_queue_frame_metadata *metadata);
const struct pulsar_d3d11_return_control *pulsar_d3d11_return_producer_control(
	pulsar_d3d11_return_producer_t *producer);

pulsar_d3d11_return_consumer_t *pulsar_d3d11_return_consumer_open(
	const wchar_t *control_name, enum pulsar_d3d11_return_lane lane, uint32_t width, uint32_t height);
void pulsar_d3d11_return_consumer_close(pulsar_d3d11_return_consumer_t *consumer);
bool pulsar_d3d11_return_consumer_read(pulsar_d3d11_return_consumer_t *consumer, uint8_t *dst,
						uint64_t *timestamp, struct video_queue_frame_metadata *metadata);
const struct pulsar_d3d11_return_control *pulsar_d3d11_return_consumer_control(
	pulsar_d3d11_return_consumer_t *consumer);

#ifdef __cplusplus
}
#endif
