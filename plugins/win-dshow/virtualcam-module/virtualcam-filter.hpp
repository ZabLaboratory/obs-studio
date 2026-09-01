#pragma once

#include <windows.h>
#include "../directshow-namespace.h"
#include "d3d11-return-transport.hpp"
#include <cstdint>
#include <string>
#include <thread>

#ifdef OBS_LEGACY
#include "../shared-memory-queue.h"
#include "../tiny-nv12-scale.h"
#include "../../../deps/libdshowcapture/src/source/output-filter.hpp"
#include "../../../deps/libdshowcapture/src/source/dshow-formats.hpp"
#include "../../../libobs/util/windows/WinHandle.hpp"
#include "../../../libobs/util/threading-windows.h"
#else
#include <shared-memory-queue.h>
#include <tiny-nv12-scale.h>
#include <source/output-filter.hpp>
#include <source/dshow-formats.hpp>
#include <util/windows/WinHandle.hpp>
#include <util/threading-windows.h>
#endif

typedef struct {
	int cx;
	int cy;
	nv12_scale_t scaler;
	const uint8_t *source_data;
	uint8_t *scaled_data;
} placeholder_t;

struct directshow_stage_timing {
	uint64_t frame_entry_monotonic_ns = 0;
	uint64_t lock_sample_data_acquired_monotonic_ns = 0;
	uint64_t queue_read_start_monotonic_ns = 0;
	uint64_t queue_read_completed_monotonic_ns = 0;
	uint64_t unlock_sample_data_completed_monotonic_ns = 0;
	uint64_t emission_monotonic_ns = 0;
	struct video_queue_read_counters queue_counters = {};
	uint32_t transport_path = PULSAR_D3D11_PATH_CPU;
	uint32_t transport_fallback = PULSAR_D3D11_FALLBACK_NOT_REQUESTED;
	int32_t transport_hresult = 0;
	uint32_t transport_lane = 0;
	uint64_t transport_epoch = 0;
	uint64_t transport_produced_sequence = 0;
	uint64_t transport_published_sequence = 0;
	uint64_t transport_consumed_sequence = 0;
	uint64_t transport_mutex_wait_ns = 0;
	uint64_t transport_fence_wait_ns = 0;
	uint64_t transport_gpu_copy_ns = 0;
	uint64_t transport_readback_ns = 0;
	uint64_t transport_frame_age_ns = 0;
};

class VCamFilter : public DShow::OutputFilter {
	std::thread th;

	video_queue_t *vq = nullptr;
	std::wstring queue_name;
	std::wstring consumer_lease_name;
	bool queue_namespace_rejected = false;
	bool program_return = false;
	bool preview_return = false;
	bool consumer_gated = false;
	pulsar_d3d11_return_consumer_t *d3d11 = nullptr;
	bool d3d11_requested = false;
	WinHandle consumer_lease;
	int queue_mode = 0;
	bool in_obs = false;
	enum queue_state prev_state = SHARED_QUEUE_STATE_INVALID;
	placeholder_t placeholder;
	uint32_t obs_cx = 0;
	uint32_t obs_cy = 0;
	uint64_t obs_interval = 0;
	uint32_t filter_cx = 0;
	uint32_t filter_cy = 0;
	DShow::VideoFormat format;
	WinHandle thread_start;
	WinHandle thread_stop;
	volatile bool active = false;

	nv12_scale_t scaler = {};
	std::string last_trace_take;

	inline bool stopped() const { return WaitForSingleObject(thread_stop, 0) != WAIT_TIMEOUT; }

	inline uint64_t GetTime();

	void Thread();
	void Frame(uint64_t ts);
	bool ShowOBSFrame(uint8_t *ptr, struct video_queue_frame_metadata *metadata,
			  struct video_queue_read_counters *counters);
	void ShowDefaultFrame(uint8_t *ptr);
	void EmitDirectShowObservation(const struct video_queue_frame_metadata &metadata,
					      const struct directshow_stage_timing &timing);
	HRESULT AcquireConsumerLease();
	void ReleaseConsumerLease();
	void UpdatePlaceholder(void);
	const int GetOutputBufferSize(void);

protected:
	const wchar_t *FilterName() const override;

public:
	VCamFilter(enum directshow_consumer_filter_kind filter_kind);
	~VCamFilter() override;

	STDMETHODIMP Pause() override;
	STDMETHODIMP Run(REFERENCE_TIME tStart) override;
	STDMETHODIMP Stop() override;
};
