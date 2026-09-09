#include "virtualcam-filter.hpp"
#include "sleepto.h"

#include <shlobj_core.h>
#include <strsafe.h>
#include <inttypes.h>
#include <util/platform.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

using namespace DShow;

extern bool initialize_placeholder();
extern const uint8_t *get_placeholder_ptr();
extern const bool get_placeholder_size(int *out_cx, int *out_cy);
extern volatile long locks;

static uint64_t trace_monotonic_ns();

/* ========================================================================= */

static std::wstring queue_name_for_filter(enum directshow_queue_namespace queue_namespace,
					  enum directshow_consumer_filter_kind filter_kind)
{
	const wchar_t *legacy_name = filter_kind == DIRECTSHOW_CONSUMER_FILTER_PREVIEW_RETURN
						   ? L"OBSPulsarPreviewReturnVideo"
						   : filter_kind == DIRECTSHOW_CONSUMER_FILTER_PROGRAM_RETURN
							     ? L"OBSPulsarProgramReturnVideo"
							     : L"OBSVirtualCamVideo";
	char runtime_id[65] = {0};
	directshow_runtime_instance_id_value(runtime_id, sizeof(runtime_id));
	if (queue_namespace != DIRECTSHOW_QUEUE_NAMESPACE_DEDICATED)
		return legacy_name;

	wchar_t wide_id[65] = {0};
	/* IDs are validated as ASCII above; avoid a libobs link from this DLL. */
	for (size_t i = 0; runtime_id[i] != '\0'; ++i)
		wide_id[i] = (wchar_t)(unsigned char)runtime_id[i];

	wchar_t name[256] = {0};
	_snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
			     L"Local\\Pulsar.%ls.%ls", wide_id,
				     filter_kind == DIRECTSHOW_CONSUMER_FILTER_PREVIEW_RETURN
					     ? L"PreviewReturnVideo"
					     : filter_kind == DIRECTSHOW_CONSUMER_FILTER_PROGRAM_RETURN
						     ? L"ProgramReturnVideo" : L"VirtualCamVideo");
	return name;
}

VCamFilter::VCamFilter(enum directshow_consumer_filter_kind filter_kind)
	: OutputFilter(),
	  program_return(false),
	  preview_return(false),
	  consumer_gated(false),
	  d3d11(nullptr),
	  d3d11_requested(false)
{
	program_return = filter_kind == DIRECTSHOW_CONSUMER_FILTER_PROGRAM_RETURN;
	preview_return = filter_kind == DIRECTSHOW_CONSUMER_FILTER_PREVIEW_RETURN;
	consumer_gated = program_return || preview_return;
	/* DirectShow is never a D3D11 capability holder.  The producer-launched
	 * private helper performs the authenticated readback and relays an NV12
	 * frame into this read-only queue. */
	d3d11_requested = false;
	const enum directshow_queue_namespace queue_namespace = directshow_queue_namespace_for_consumer(filter_kind);
	queue_namespace_rejected = queue_namespace == DIRECTSHOW_QUEUE_NAMESPACE_REJECT;
	queue_name = queue_name_for_filter(queue_namespace, filter_kind);
	if (consumer_gated && !queue_namespace_rejected)
		consumer_lease_name = queue_name + L".ConsumerActive";
	if (queue_namespace_rejected) {
		OutputDebugStringW(L"[pulsar-directshow] queue namespace rejected; consumer is disabled\n");
	}
	thread_start = CreateEvent(nullptr, true, false, nullptr);
	thread_stop = CreateEvent(nullptr, true, false, nullptr);

	format = VideoFormat::NV12;

	placeholder.scaled_data = nullptr;

	/* ---------------------------------------- */
	/* detect if this filter is within obs      */

	wchar_t file[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, file, MAX_PATH)) {
		file[0] = 0;
	}

#ifdef _WIN64
	const wchar_t *obs_process = L"obs64.exe";
#else
	const wchar_t *obs_process = L"obs32.exe";
#endif

	in_obs = !!wcsstr(file, obs_process);

	/* ---------------------------------------- */
	/* add last/current obs res/interval        */

	uint32_t new_obs_cx = obs_cx;
	uint32_t new_obs_cy = obs_cy;
	uint64_t new_obs_interval = obs_interval;

	if (!queue_namespace_rejected)
		vq = video_queue_open_named(queue_name.c_str());
	if (vq) {
		if (video_queue_state(vq) == SHARED_QUEUE_STATE_READY) {
			video_queue_get_info(vq, &new_obs_cx, &new_obs_cy, &new_obs_interval);
		}

		/* don't keep it open until the filter actually starts */
		video_queue_close(vq);
		vq = nullptr;
	} else {
		wchar_t res_file[MAX_PATH];
		SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, res_file);
		StringCbCat(res_file, sizeof(res_file), L"\\obs-virtualcam.txt");

		HANDLE file = CreateFileW(res_file, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file) {
			char res[128];
			DWORD len = 0;

			if (ReadFile(file, res, sizeof(res) - 1, &len, nullptr)) {
				res[len] = 0;
				int vals = sscanf(res, "%" PRIu32 "x%" PRIu32 "x%" PRIu64, &new_obs_cx, &new_obs_cy,
						  &new_obs_interval);
				if (vals != 3) {
					new_obs_cx = obs_cx;
					new_obs_cy = obs_cy;
					new_obs_interval = obs_interval;
				}
			}

			CloseHandle(file);
		}
	}
	/* ProgramReturn/PreviewReturn outputs are consumer-gated: before a
	 * DirectShow graph attaches, their producer intentionally does not publish
	 * a frame, so the queue remains STARTING.  Do not leave the OutputPin with
	 * a zero-sized media type in that bootstrap window; that creates a producer
	 * / consumer deadlock because FFmpeg cannot attach to acquire the consumer
	 * lease.  Pulsar's return contract is fixed at 1920x1080/60, and the queue
	 * will replace these defaults on the first READY frame. */
	if (consumer_gated && (!new_obs_cx || !new_obs_cy || !new_obs_interval)) {
		new_obs_cx = PULSAR_D3D11_RETURN_WIDTH;
		new_obs_cy = PULSAR_D3D11_RETURN_HEIGHT;
		new_obs_interval = 10000000ULL / 60ULL;
	}

	if (new_obs_cx != obs_cx || new_obs_cy != obs_cy || new_obs_interval != obs_interval) {
		AddVideoFormat(VideoFormat::NV12, new_obs_cx, new_obs_cy, new_obs_interval);
		AddVideoFormat(VideoFormat::I420, new_obs_cx, new_obs_cy, new_obs_interval);
		AddVideoFormat(VideoFormat::YUY2, new_obs_cx, new_obs_cy, new_obs_interval);
		SetVideoFormat(VideoFormat::NV12, new_obs_cx, new_obs_cy, new_obs_interval);

		obs_cx = new_obs_cx;
		obs_cy = new_obs_cy;
		obs_interval = new_obs_interval;
	}

	/* ---------------------------------------- */

	th = std::thread([this] { Thread(); });

	AddRef();
	os_atomic_inc_long(&locks);
}

VCamFilter::~VCamFilter()
{
	ReleaseConsumerLease();
	SetEvent(thread_stop);
	if (th.joinable())
		th.join();
	ReleaseConsumerRegistrationPipe();
	d3d11 = nullptr;
	video_queue_close(vq);

	if (placeholder.scaled_data)
		free(placeholder.scaled_data);

	os_atomic_dec_long(&locks);
}

HRESULT VCamFilter::AcquireConsumerLease()
{
	if (!consumer_gated || consumer_lease.Valid())
		return S_OK;

	consumer_lease = CreateEventW(nullptr, TRUE, FALSE, consumer_lease_name.c_str());
	const DWORD lease_error = GetLastError();
	if (!consumer_lease.Valid() || lease_error == ERROR_ALREADY_EXISTS) {
		if (consumer_lease.Valid())
			consumer_lease = nullptr;
		const DWORD error = lease_error;
		OutputDebugStringW(L"[pulsar-directshow] failed to acquire return consumer lease\n");
		return HRESULT_FROM_WIN32(error ? error : ERROR_OPEN_FAILED);
	}
	return S_OK;
}

void VCamFilter::ReleaseConsumerLease()
{
	consumer_lease = nullptr;
}

bool VCamFilter::OpenConsumerRegistrationPipe()
{
	if (!consumer_gated || !vq || consumer_registration_pipe != INVALID_HANDLE_VALUE)
		return consumer_registration_pipe != INVALID_HANDLE_VALUE;
	uint64_t challenge = 0;
	if (!video_queue_get_challenge(vq, &challenge))
		return false;
	wchar_t pipe_name[256] = {0};
	const wchar_t *role = preview_return ? L"PreviewReturn" : L"ProgramReturn";
	_snwprintf_s(pipe_name, sizeof(pipe_name) / sizeof(pipe_name[0]), _TRUNCATE,
			     L"\\\\.\\pipe\\PulsarReturn.%ls.%016llx", role, (unsigned long long)challenge);
	consumer_registration_pipe = CreateFileW(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	return consumer_registration_pipe != INVALID_HANDLE_VALUE;
}

void VCamFilter::ReleaseConsumerRegistrationPipe()
{
	if (consumer_registration_pipe != INVALID_HANDLE_VALUE) {
		CloseHandle(consumer_registration_pipe);
		consumer_registration_pipe = INVALID_HANDLE_VALUE;
	}
}

const wchar_t *VCamFilter::FilterName() const
{
	return L"VCamFilter";
}

STDMETHODIMP VCamFilter::Pause()
{
	HRESULT hr;

	hr = OutputFilter::Pause();
	if (FAILED(hr)) {
		return hr;
	}
	hr = AcquireConsumerLease();
	if (FAILED(hr))
		return hr;

	os_atomic_set_bool(&active, true);
	SetEvent(thread_start);
	return S_OK;
}

STDMETHODIMP VCamFilter::Run(REFERENCE_TIME tStart)
{
	HRESULT hr = AcquireConsumerLease();
	if (FAILED(hr))
		return hr;
	os_atomic_set_bool(&active, true);
	return OutputFilter::Run(tStart);
}

STDMETHODIMP VCamFilter::Stop()
{
	os_atomic_set_bool(&active, false);
	ReleaseConsumerLease();
	ReleaseConsumerRegistrationPipe();
	return OutputFilter::Stop();
}

inline uint64_t VCamFilter::GetTime()
{
	if (!!clock) {
		REFERENCE_TIME rt;
		HRESULT hr = clock->GetTime(&rt);
		if (SUCCEEDED(hr)) {
			return (uint64_t)rt;
		}
	}

	return gettime_100ns();
}

void VCamFilter::Thread()
{
	HANDLE h[2] = {thread_start, thread_stop};
	DWORD ret = WaitForMultipleObjects(2, h, false, INFINITE);
	if (ret != WAIT_OBJECT_0)
		return;

	uint64_t cur_time = gettime_100ns();
	uint64_t filter_time = GetTime();

	obs_cx = (uint32_t)GetCX();
	obs_cy = (uint32_t)GetCY();
	obs_interval = (uint64_t)GetInterval();
	filter_cx = obs_cx;
	filter_cy = obs_cy;

	/* ---------------------------------------- */
	/* load placeholder image                   */

	if (initialize_placeholder()) {
		placeholder.source_data = get_placeholder_ptr();
		get_placeholder_size(&placeholder.cx, &placeholder.cy);
	} else {
		placeholder.source_data = nullptr;
	}

	/* Created dynamically based on output resolution changes */
	placeholder.scaled_data = nullptr;

	nv12_scale_init(&scaler, TARGET_FORMAT_NV12, obs_cx, obs_cy, obs_cx, obs_cy);
	nv12_scale_init(&placeholder.scaler, TARGET_FORMAT_NV12, obs_cx, obs_cy, placeholder.cx, placeholder.cy);

	UpdatePlaceholder();

	while (!stopped()) {
		if (os_atomic_load_bool(&active))
			Frame(filter_time);
		sleepto_100ns(cur_time += obs_interval);
		filter_time += obs_interval;
	}
}

void VCamFilter::Frame(uint64_t ts)
{
	const uint64_t frame_entry_monotonic_ns = trace_monotonic_ns();
	if (queue_namespace_rejected) {
		uint8_t *ptr;
		if (LockSampleData(&ptr)) {
			ShowDefaultFrame(ptr);
			UnlockSampleData(ts, ts + obs_interval);
		}
		return;
	}

	uint32_t new_obs_cx = obs_cx;
	uint32_t new_obs_cy = obs_cy;
	uint64_t new_obs_interval = obs_interval;

	/* cx, cy and interval are the resolution and frame rate of the
	   virtual camera _source_, ie OBS' output. Do not confuse cx / cy
	   with GetCX() / GetCY() / GetInterval() which return the virtualcam
	   filter output! */

	if (!vq) {
		vq = video_queue_open_named(queue_name.c_str());
	}
	if (consumer_gated && vq)
		OpenConsumerRegistrationPipe();

	enum queue_state state = video_queue_state(vq);
	if (state != prev_state) {
		if (state == SHARED_QUEUE_STATE_READY) {
			/* The virtualcam output from OBS has started, get
			   the actual cx / cy of the data stream */
			video_queue_get_info(vq, &new_obs_cx, &new_obs_cy, &new_obs_interval);
		} else if (state == SHARED_QUEUE_STATE_STOPPING) {
			ReleaseConsumerRegistrationPipe();
			video_queue_close(vq);
			vq = nullptr;
		}

		prev_state = state;
	}

	uint32_t new_filter_cx = (uint32_t)GetCX();
	uint32_t new_filter_cy = (uint32_t)GetCY();

	if (state != SHARED_QUEUE_STATE_READY) {
		/* Virtualcam output not yet started, assume it's
		   the same resolution as the filter output */
		new_obs_cx = new_filter_cx;
		new_obs_cy = new_filter_cy;
		new_obs_interval = GetInterval();
	}

	if (new_obs_cx != obs_cx || new_obs_cy != obs_cy || new_obs_interval != obs_interval) {
		/* The res / FPS of the video coming from OBS has
		   changed, update parameters as needed */
		if (in_obs) {
			/* If the vcam is being used inside obs, adjust
			   the format we present to match */
			SetVideoFormat(GetVideoFormat(), new_obs_cx, new_obs_cy, new_obs_interval);

			/* Update the new filter size immediately since we
			   know it just changed above */
			new_filter_cx = new_obs_cx;
			new_filter_cy = new_obs_cy;
		}

		/* Re-initialize the main scaler to use the new resolution */
		nv12_scale_init(&scaler, scaler.format, new_filter_cx, new_filter_cy, new_obs_cx, new_obs_cy);

		obs_cx = new_obs_cx;
		obs_cy = new_obs_cy;
		obs_interval = new_obs_interval;
		filter_cx = new_filter_cx;
		filter_cy = new_filter_cy;

		UpdatePlaceholder();

	} else if (new_filter_cx != filter_cx || new_filter_cy != filter_cy) {
		filter_cx = new_filter_cx;
		filter_cy = new_filter_cy;

		/* Re-initialize the main scaler to use the new resolution */
		nv12_scale_init(&scaler, scaler.format, new_filter_cx, new_filter_cy, new_obs_cx, new_obs_cy);

		UpdatePlaceholder();
	}

	VideoFormat current_format = GetVideoFormat();

	if (current_format != format) {
		/* The output format changed, update the scalers */
		if (current_format == VideoFormat::I420)
			scaler.format = placeholder.scaler.format = TARGET_FORMAT_I420;
		else if (current_format == VideoFormat::YUY2)
			scaler.format = placeholder.scaler.format = TARGET_FORMAT_YUY2;
		else
			scaler.format = placeholder.scaler.format = TARGET_FORMAT_NV12;

		format = current_format;

		UpdatePlaceholder();
	}

	/* Actual output */
	uint8_t *ptr;
	struct video_queue_frame_metadata metadata = {};
	struct directshow_stage_timing timing = {};
	timing.frame_entry_monotonic_ns = frame_entry_monotonic_ns;
	bool consumed_program_frame = false;
	if (LockSampleData(&ptr)) {
		timing.lock_sample_data_acquired_monotonic_ns = trace_monotonic_ns();
		if (state == SHARED_QUEUE_STATE_READY)
			timing.queue_read_start_monotonic_ns = trace_monotonic_ns();
		if (state == SHARED_QUEUE_STATE_READY)
			consumed_program_frame = ShowOBSFrame(ptr, &metadata, &timing.queue_counters);
		if (state == SHARED_QUEUE_STATE_READY)
			timing.queue_read_completed_monotonic_ns = trace_monotonic_ns();
		if (!consumed_program_frame)
			ShowDefaultFrame(ptr);

		UnlockSampleData(ts, ts + obs_interval);
		timing.unlock_sample_data_completed_monotonic_ns = trace_monotonic_ns();
	}

	if (consumed_program_frame && program_return)
		EmitDirectShowObservation(metadata, timing);
}

bool VCamFilter::ShowOBSFrame(uint8_t *ptr, struct video_queue_frame_metadata *metadata,
				      struct video_queue_read_counters *counters)
{
	uint64_t temp = 0;
	const bool consumed = video_queue_read_ex(vq, &scaler, ptr, &temp, metadata);
	video_queue_get_read_counters(vq, counters);
	return consumed;
}

static uint64_t trace_monotonic_ns()
{
	static LARGE_INTEGER frequency = [] {
		LARGE_INTEGER value = {};
		QueryPerformanceFrequency(&value);
		return value;
	}();
	LARGE_INTEGER counter = {};
	QueryPerformanceCounter(&counter);
	if (frequency.QuadPart <= 0)
		return 0;
	const uint64_t ticks = (uint64_t)counter.QuadPart;
	const uint64_t hz = (uint64_t)frequency.QuadPart;
	return (ticks / hz) * 1000000000ULL + ((ticks % hz) * 1000000000ULL) / hz;
}

static bool trace_prepare_identifier(const char *value, size_t capacity, std::string &raw,
				     std::string &escaped)
{
	raw.clear();
	escaped.clear();
	if (!value || !capacity)
		return false;

	const void *terminator = std::memchr(value, '\0', capacity);
	if (!terminator)
		return false;

	const size_t length = static_cast<const char *>(terminator) - value;
	raw.assign(value, length);
	escaped.reserve(length);
	for (const unsigned char ch : raw) {
		switch (ch) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (ch < 0x20)
				return false;
			escaped.push_back((char)ch);
			break;
		}
	}
	return true;
}

static bool append_trace_line(const std::string &line, const std::string &runtime_id)
{
	/* DirectShow observations are consumer-owned and cannot be added to the
	 * producer's authenticated stream without the operator key.  Emit them to
	 * the explicit out-of-band sidecar; the probe validates and re-signs them
	 * during post-stop fusion. */
	const char *path = getenv("PULSAR_DIRECTSHOW_TRACE_PATH");
	if (!path || !*path || runtime_id.empty())
		return false;

	const std::string mutex_name = std::string("Local\\Pulsar.") + runtime_id + ".Trace";
	HANDLE mutex = CreateMutexA(nullptr, FALSE, mutex_name.c_str());
	if (!mutex)
		return false;
	const DWORD wait_result = WaitForSingleObject(mutex, INFINITE);
	if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
		CloseHandle(mutex);
		return false;
	}

	bool appended = false;
	HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		const std::string payload = line + "\n";
		DWORD written = 0;
		appended = WriteFile(file, payload.data(), (DWORD)payload.size(), &written, nullptr) != FALSE &&
				written == payload.size();
		CloseHandle(file);
	}

	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return appended;
}

void VCamFilter::EmitDirectShowObservation(const struct video_queue_frame_metadata &metadata,
						   const struct directshow_stage_timing &timing)
{
	std::string runtime_id;
	std::string runtime_id_json;
	std::string command_id;
	std::string command_id_json;
	std::string intent_id;
	std::string intent_id_json;
	std::string take_command_id;
	std::string take_command_id_json;
	if (!metadata.valid ||
	    !trace_prepare_identifier(metadata.runtime_instance_id, sizeof(metadata.runtime_instance_id), runtime_id,
				      runtime_id_json) ||
	    !trace_prepare_identifier(metadata.command_id, sizeof(metadata.command_id), command_id, command_id_json) ||
	    !trace_prepare_identifier(metadata.intent_id, sizeof(metadata.intent_id), intent_id, intent_id_json) ||
	    !trace_prepare_identifier(metadata.take_command_id, sizeof(metadata.take_command_id), take_command_id,
				      take_command_id_json) ||
	    take_command_id.empty() || last_trace_take == take_command_id)
		return;

	const uint64_t emission_monotonic_ns = trace_monotonic_ns();
	const bool timing_complete = timing.frame_entry_monotonic_ns > 0 &&
		timing.lock_sample_data_acquired_monotonic_ns > timing.frame_entry_monotonic_ns &&
		timing.queue_read_start_monotonic_ns > timing.lock_sample_data_acquired_monotonic_ns &&
		timing.queue_read_completed_monotonic_ns > timing.queue_read_start_monotonic_ns &&
		timing.unlock_sample_data_completed_monotonic_ns > timing.queue_read_completed_monotonic_ns &&
		emission_monotonic_ns > timing.unlock_sample_data_completed_monotonic_ns;
	const uint64_t observed_at_monotonic_ns = timing.unlock_sample_data_completed_monotonic_ns;

	std::ostringstream observation;
	observation << "{\"record_type\":\"observation\",\"boundary\":\"directshow_return\","
			<< "\"clock_domain\":\"monotonic_ns\",\"runtime_instance_id\":\""
			<< runtime_id_json << "\",\"command_id\":\""
			<< command_id_json << "\",\"intent_id\":\""
			<< intent_id_json << "\",\"take_command_id\":\""
			<< take_command_id_json << "\",\"revisions\":{"
			<< "\"program\":" << metadata.program_revision << ",\"preview\":"
			<< metadata.preview_revision << ",\"role_map\":" << metadata.role_map_revision << "},"
			<< "\"frame_id\":" << metadata.frame_id << ",\"pts_ns\":" << metadata.pts_ns
			<< ",\"observed_at_monotonic_ns\":" << observed_at_monotonic_ns
			<< ",\"valid\":true,\"program_frame\":true,\"surface\":\"ProgramReturn\","
			<< "\"consumer\":\"DirectShow\",\"queue_counters\":{"
			<< "\"gap_count\":" << timing.queue_counters.gap_count
			<< ",\"duplicate_count\":" << timing.queue_counters.duplicate_count
			<< ",\"retry_count\":" << timing.queue_counters.retry_count
			<< ",\"torn_count\":" << timing.queue_counters.torn_count << "},\"transport\":{";
	observation << "\"path\":" << timing.transport_path
			<< ",\"fallback_reason\":" << timing.transport_fallback
			<< ",\"fallback_hresult\":" << timing.transport_hresult
			<< ",\"lane\":" << timing.transport_lane
			<< ",\"epoch\":" << timing.transport_epoch
			<< ",\"produced_sequence\":" << timing.transport_produced_sequence
			<< ",\"published_sequence\":" << timing.transport_published_sequence
			<< ",\"consumed_sequence\":" << timing.transport_consumed_sequence
			<< ",\"mutex_wait_ns\":" << timing.transport_mutex_wait_ns
			<< ",\"fence_wait_ns\":" << timing.transport_fence_wait_ns
			<< ",\"gpu_copy_ns\":" << timing.transport_gpu_copy_ns
			<< ",\"readback_ns\":" << timing.transport_readback_ns
			<< ",\"frame_age_ns\":" << timing.transport_frame_age_ns << "}}";
	if (timing_complete) {
		observation.seekp(-1, std::ios_base::end);
		observation << ",\"frame_entry_monotonic_ns\":" << timing.frame_entry_monotonic_ns
				<< ",\"lock_sample_data_acquired_monotonic_ns\":"
				<< timing.lock_sample_data_acquired_monotonic_ns
				<< ",\"queue_read_start_monotonic_ns\":"
				<< timing.queue_read_start_monotonic_ns
				<< ",\"queue_read_completed_monotonic_ns\":"
				<< timing.queue_read_completed_monotonic_ns
				<< ",\"unlock_sample_data_completed_monotonic_ns\":"
				<< timing.unlock_sample_data_completed_monotonic_ns
				<< ",\"emission_monotonic_ns\":" << emission_monotonic_ns << "}";
	}
	if (append_trace_line(observation.str(), runtime_id))
		last_trace_take = take_command_id;
}

void VCamFilter::ShowDefaultFrame(uint8_t *ptr)
{
	if (placeholder.scaled_data) {
		memcpy(ptr, placeholder.scaled_data, GetOutputBufferSize());
	} else {
		memset(ptr, 127, GetOutputBufferSize());
	}
}

/* Called when the output resolution or format has changed to re-scale
   the placeholder graphic into the placeholder.scaled_data buffer. */
void VCamFilter::UpdatePlaceholder(void)
{
	if (!placeholder.source_data)
		return;

	if (placeholder.scaled_data)
		free(placeholder.scaled_data);

	placeholder.scaled_data = (uint8_t *)malloc(GetOutputBufferSize());
	if (!placeholder.scaled_data)
		return;

	if (placeholder.cx == GetCX() && placeholder.cy == GetCY() && placeholder.scaler.format == TARGET_FORMAT_NV12) {
		/* No scaling necessary if it matches exactly */
		memcpy(placeholder.scaled_data, placeholder.source_data, GetOutputBufferSize());
	} else {
		nv12_scale_init(&placeholder.scaler, placeholder.scaler.format, GetCX(), GetCY(), placeholder.cx,
				placeholder.cy);
		nv12_do_scale(&placeholder.scaler, placeholder.scaled_data, placeholder.source_data);
	}
}

/* Calculate the size of the output buffer based on the filter's
   resolution and format */
const int VCamFilter::GetOutputBufferSize(void)
{
	int bits = VFormatBits(format);
	return GetCX() * GetCY() * bits / 8;
}
