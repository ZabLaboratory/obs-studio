#include "d3d11-return-transport.hpp"

#include "../../../shared/obs-shared-memory-queue/shared-memory-queue.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWaitMs = 2;
constexpr uint64_t kMaxSequence = UINT64_MAX - 1;
constexpr uint32_t kSharedReadWrite = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;

static uint64_t now_ns()
{
	LARGE_INTEGER frequency = {};
	LARGE_INTEGER counter = {};
	if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0)
		return 0;
	const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
	const uint64_t hz = static_cast<uint64_t>(frequency.QuadPart);
	return (ticks / hz) * 1000000000ULL + ((ticks % hz) * 1000000000ULL) / hz;
}

static uint64_t load_u64(const volatile uint64_t *value)
{
	return static_cast<uint64_t>(InterlockedCompareExchange64((volatile LONG64 *)value, 0, 0));
}

static void store_u64(volatile uint64_t *value, uint64_t next)
{
	InterlockedExchange64((volatile LONG64 *)value, static_cast<LONG64>(next));
}

static uint32_t process_session(DWORD pid)
{
	DWORD session = 0;
	return ProcessIdToSessionId(pid, &session) ? session : UINT32_MAX;
}

static void set_fallback(pulsar_d3d11_return_control *control, enum pulsar_d3d11_return_fallback reason,
				 HRESULT hr)
{
	if (!control)
		return;
	control->selected_path = PULSAR_D3D11_PATH_CPU;
	control->fallback_reason = static_cast<uint32_t>(reason);
	control->fallback_hresult = static_cast<int32_t>(hr);
	InterlockedExchange((volatile LONG *)&control->consumer_ready, 0);
}

static bool valid_nv12(uint32_t width, uint32_t height, uint8_t **data, uint32_t *linesize)
{
	return width == PULSAR_D3D11_RETURN_WIDTH && height == PULSAR_D3D11_RETURN_HEIGHT && data && linesize &&
		data[0] && data[1] && linesize[0] >= width && linesize[1] >= width && (width & 1U) == 0 &&
		(height & 1U) == 0;
}

static bool pack_nv12(uint32_t width, uint32_t height, uint8_t **data, uint32_t *linesize,
			      std::vector<uint8_t> &packed)
{
	if (!valid_nv12(width, height, data, linesize))
		return false;
	packed.resize(static_cast<size_t>(width) * height * 3 / 2);
	for (uint32_t row = 0; row < height; ++row)
		memcpy(packed.data() + static_cast<size_t>(row) * width, data[0] + static_cast<size_t>(row) * linesize[0], width);
	uint8_t *uv = packed.data() + static_cast<size_t>(width) * height;
	for (uint32_t row = 0; row < height / 2; ++row)
		memcpy(uv + static_cast<size_t>(row) * width, data[1] + static_cast<size_t>(row) * linesize[1], width);
	return true;
}

static std::wstring control_name(const wchar_t *name)
{
	return name ? std::wstring(name) + L".D3D11" : std::wstring();
}

/* Keep the transport opt-in invariant inside the ABI boundary as well as at
 * the OBS call sites. This makes a stale module/object pointer fail closed if
 * the process is running the default CPU transport (or the environment was
 * changed between construction and teardown). */
static bool d3d11_transport_requested()
{
	const char *transport = getenv("PULSAR_RETURN_TRANSPORT");
	return transport && strcmp(transport, "d3d11") == 0;
}

static HRESULT create_device_for_luid(const pulsar_d3d11_return_luid *wanted, ComPtr<ID3D11Device> &device,
					     ComPtr<ID3D11DeviceContext> &context, pulsar_d3d11_return_luid *actual)
{
	ComPtr<IDXGIFactory1> factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(hr))
		return hr;

	ComPtr<IDXGIAdapter1> selected;
	for (UINT index = 0;; ++index) {
		ComPtr<IDXGIAdapter1> candidate;
		if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			continue;
		if (wanted && (desc.AdapterLuid.LowPart != wanted->low || desc.AdapterLuid.HighPart != wanted->high))
			continue;
		selected = candidate;
		if (actual) {
			actual->low = desc.AdapterLuid.LowPart;
			actual->high = desc.AdapterLuid.HighPart;
		}
		break;
	}
	if (!selected)
		return DXGI_ERROR_NOT_FOUND;

	constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
						D3D_FEATURE_LEVEL_10_0};
	return D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
					 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
}

static HRESULT create_shared_texture(ID3D11Device *device, ID3D11Texture2D **texture, HANDLE *handle,
					    ComPtr<IDXGIKeyedMutex> &mutex)
{
	if (!device || !texture || !handle)
		return E_INVALIDARG;
	*texture = nullptr;
	*handle = nullptr;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = PULSAR_D3D11_RETURN_WIDTH;
	desc.Height = PULSAR_D3D11_RETURN_HEIGHT;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ComPtr<ID3D11Texture2D> created;
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, &created);
	if (FAILED(hr))
		return hr;
	ComPtr<IDXGIResource1> resource;
	hr = created.As(&resource);
	if (FAILED(hr))
		return hr;
	hr = resource->CreateSharedHandle(nullptr, kSharedReadWrite, nullptr, handle);
	if (FAILED(hr))
		return hr;
	hr = created.As(&mutex);
	if (FAILED(hr)) {
		CloseHandle(*handle);
		*handle = nullptr;
		return hr;
	}
	*texture = created.Detach();
	return S_OK;
}

static HRESULT duplicate_handle_for_pid(HANDLE source, DWORD pid, uint64_t *value)
{
	if (!source || !value || !pid)
		return E_INVALIDARG;
	HANDLE target_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
	if (!target_process)
		return HRESULT_FROM_WIN32(GetLastError());
	HANDLE duplicated = nullptr;
	const BOOL ok = DuplicateHandle(GetCurrentProcess(), source, target_process, &duplicated, 0, FALSE,
						DUPLICATE_SAME_ACCESS);
	const DWORD error = GetLastError();
	CloseHandle(target_process);
	if (!ok)
		return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_HANDLE);
	*value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(duplicated));
	return S_OK;
}

} // namespace

struct pulsar_d3d11_return_producer {
	HANDLE mapping = nullptr;
	pulsar_d3d11_return_control *control = nullptr;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> textures[PULSAR_D3D11_RETURN_SLOT_COUNT];
	ComPtr<IDXGIKeyedMutex> mutexes[PULSAR_D3D11_RETURN_SLOT_COUNT];
	HANDLE source_handles[PULSAR_D3D11_RETURN_SLOT_COUNT] = {};
	uint32_t consumer_pid = 0;
	uint64_t epoch = 1;
	uint64_t sequence = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> upload;
};

struct pulsar_d3d11_return_consumer {
	HANDLE mapping = nullptr;
	pulsar_d3d11_return_control *control = nullptr;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> textures[PULSAR_D3D11_RETURN_SLOT_COUNT];
	ComPtr<IDXGIKeyedMutex> mutexes[PULSAR_D3D11_RETURN_SLOT_COUNT];
	ComPtr<ID3D11Texture2D> staging;
	ComPtr<ID3D11Query> query;
	uint32_t width = 0;
	uint32_t height = 0;
	bool active = false;
};

extern "C" pulsar_d3d11_return_producer_t *pulsar_d3d11_return_producer_create(
	const wchar_t *name, enum pulsar_d3d11_return_lane lane, uint32_t width, uint32_t height)
{
	if (!d3d11_transport_requested() || !name || !*name ||
		(lane != PULSAR_D3D11_PROGRAM_RETURN && lane != PULSAR_D3D11_PREVIEW_RETURN) ||
		width != PULSAR_D3D11_RETURN_WIDTH || height != PULSAR_D3D11_RETURN_HEIGHT)
		return nullptr;
	const std::wstring map_name = control_name(name);
	auto *producer = new pulsar_d3d11_return_producer();
	producer->width = width;
	producer->height = height;
	producer->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
							 static_cast<DWORD>(sizeof(pulsar_d3d11_return_control)), map_name.c_str());
	const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
	if (!producer->mapping) {
		delete producer;
		return nullptr;
	}
	producer->control = static_cast<pulsar_d3d11_return_control *>(MapViewOfFile(
		producer->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(pulsar_d3d11_return_control)));
	if (!producer->control) {
		CloseHandle(producer->mapping);
		delete producer;
		return nullptr;
	}
	// Reject every pre-existing mapping. producer_pid is not authoritative:
	// a squatter can win the CreateFileMappingW race with a zeroed block.
	if (existed) {
		UnmapViewOfFile(producer->control);
		CloseHandle(producer->mapping);
		delete producer;
		return nullptr;
	}
	memset(producer->control, 0, sizeof(*producer->control));
	producer->control->abi_version = PULSAR_D3D11_RETURN_ABI_VERSION;
	producer->control->lane = lane;
	producer->control->width = width;
	producer->control->height = height;
	producer->control->producer_pid = GetCurrentProcessId();
	producer->control->producer_session = process_session(producer->control->producer_pid);
	producer->control->selected_path = PULSAR_D3D11_PATH_CPU;
	producer->control->fallback_reason = PULSAR_D3D11_FALLBACK_CAPABILITY;
	producer->control->fallback_hresult = static_cast<int32_t>(E_PENDING);
	producer->control->epoch = producer->epoch;
	return producer;
}

static void producer_release_ring(pulsar_d3d11_return_producer *producer)
{
	if (!producer)
		return;
	for (HANDLE &handle : producer->source_handles) {
		if (handle) {
			CloseHandle(handle);
			handle = nullptr;
		}
	}
	for (auto &texture : producer->textures)
		texture.Reset();
	for (auto &mutex : producer->mutexes)
		mutex.Reset();
	producer->device.Reset();
	producer->context.Reset();
	producer->consumer_pid = 0;
}

extern "C" void pulsar_d3d11_return_producer_close(pulsar_d3d11_return_producer_t *producer)
{
	if (!producer)
		return;
	if (producer->control)
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_INTEROP, E_ABORT);
	producer_release_ring(producer);
	if (producer->control)
		UnmapViewOfFile(producer->control);
	if (producer->mapping)
		CloseHandle(producer->mapping);
	delete producer;
}

static bool producer_prepare_ring(pulsar_d3d11_return_producer *producer)
{
	if (!producer || !producer->control)
		return false;
	const uint32_t pid = producer->control->consumer_pid;
	if (!pid)
		return false;
	if (producer->consumer_pid == pid && producer->control->selected_path == PULSAR_D3D11_PATH_SHARED_TEXTURE &&
		producer->control->consumer_ready)
		return true;
	if (producer->consumer_pid == pid && producer->control->selected_path == PULSAR_D3D11_PATH_SHARED_TEXTURE)
		return false;
	if (producer->consumer_pid == pid && producer->control->fallback_reason != PULSAR_D3D11_FALLBACK_NONE)
		return false;
	producer_release_ring(producer);
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	pulsar_d3d11_return_luid luid = {};
	HRESULT hr = create_device_for_luid(nullptr, device, context, &luid);
	if (FAILED(hr)) {
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_CAPABILITY, hr);
		producer->consumer_pid = pid;
		return false;
	}
	producer->device = device;
	producer->context = context;
	producer->control->adapter_luid = luid;
	++producer->epoch;
	for (uint32_t i = 0; i < PULSAR_D3D11_RETURN_SLOT_COUNT; ++i) {
		ID3D11Texture2D *texture = nullptr;
		HANDLE handle = nullptr;
		ComPtr<IDXGIKeyedMutex> mutex;
		hr = create_shared_texture(producer->device.Get(), &texture, &handle, mutex);
		if (FAILED(hr))
			break;
		uint64_t duplicated = 0;
		hr = duplicate_handle_for_pid(handle, pid, &duplicated);
		if (FAILED(hr)) {
			texture->Release();
			CloseHandle(handle);
			break;
		}
		producer->textures[i].Attach(texture);
		producer->mutexes[i] = mutex;
		producer->source_handles[i] = handle;
		producer->control->slots[i].handle.value = duplicated;
		producer->control->slots[i].epoch = producer->epoch;
		producer->control->slots[i].sequence = 0;
	}
	if (FAILED(hr)) {
		producer_release_ring(producer);
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_INTEROP, hr);
		producer->consumer_pid = pid;
		return false;
	}
	producer->consumer_pid = pid;
	producer->control->epoch = producer->epoch;
	producer->control->selected_path = PULSAR_D3D11_PATH_SHARED_TEXTURE;
	producer->control->fallback_reason = PULSAR_D3D11_FALLBACK_NONE;
	producer->control->fallback_hresult = 0;
	InterlockedExchange((volatile LONG *)&producer->control->consumer_ready, 1);
	return true;
}

extern "C" bool pulsar_d3d11_return_producer_ready(pulsar_d3d11_return_producer_t *producer)
{
	return producer_prepare_ring(producer);
}

extern "C" bool pulsar_d3d11_return_producer_write(pulsar_d3d11_return_producer_t *producer, uint8_t **data,
								 uint32_t *linesize, uint64_t timestamp,
								 const struct video_queue_frame_metadata *metadata)
{
	if (!producer || !producer_prepare_ring(producer) || producer->sequence >= kMaxSequence)
		return false;
	if (!pack_nv12(producer->width, producer->height, data, linesize, producer->upload)) {
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_FORMAT, E_INVALIDARG);
		return false;
	}
	const uint32_t index = static_cast<uint32_t>(producer->sequence % PULSAR_D3D11_RETURN_SLOT_COUNT);
	const uint64_t wait_start = now_ns();
	const HRESULT acquire = producer->mutexes[index]->AcquireSync(0, 0);
	const uint64_t wait_end = now_ns();
	producer->control->mutex_wait_ns = wait_end >= wait_start ? wait_end - wait_start : 0;
	if (acquire == WAIT_TIMEOUT) {
		++producer->control->retry_count;
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_TIMEOUT, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
		return false;
	}
	if (FAILED(acquire)) {
		set_fallback(producer->control, PULSAR_D3D11_FALLBACK_DEVICE_REMOVED, acquire);
		return false;
	}
	const uint64_t copy_start = now_ns();
	producer->context->UpdateSubresource(producer->textures[index].Get(), 0, nullptr, producer->upload.data(),
						 producer->width, 0);
	producer->context->Flush();
	const uint64_t copy_end = now_ns();
	producer->control->gpu_copy_ns = copy_end >= copy_start ? copy_end - copy_start : 0;
	const uint64_t sequence = ++producer->sequence;
	auto &slot = producer->control->slots[index];
	slot.timestamp = timestamp;
	if (metadata) {
		slot.frame_id = metadata->frame_id;
		slot.pts_ns = metadata->pts_ns;
		slot.server_seq = metadata->server_seq;
		slot.program_revision = metadata->program_revision;
		slot.preview_revision = metadata->preview_revision;
		slot.role_map_revision = metadata->role_map_revision;
		slot.valid = metadata->valid;
		memcpy(slot.runtime_instance_id, metadata->runtime_instance_id, sizeof(slot.runtime_instance_id));
		memcpy(slot.command_id, metadata->command_id, sizeof(slot.command_id));
		memcpy(slot.intent_id, metadata->intent_id, sizeof(slot.intent_id));
		memcpy(slot.take_command_id, metadata->take_command_id, sizeof(slot.take_command_id));
	} else {
		slot.frame_id = slot.pts_ns = slot.server_seq = slot.program_revision = slot.preview_revision =
			slot.role_map_revision = 0;
		slot.valid = 0;
		memset(slot.runtime_instance_id, 0, sizeof(slot.runtime_instance_id));
		memset(slot.command_id, 0, sizeof(slot.command_id));
		memset(slot.intent_id, 0, sizeof(slot.intent_id));
		memset(slot.take_command_id, 0, sizeof(slot.take_command_id));
	}
	slot.sequence = sequence;
	MemoryBarrier();
	producer->mutexes[index]->ReleaseSync(1);
	store_u64((volatile uint64_t *)&producer->control->produced_sequence, sequence);
	store_u64((volatile uint64_t *)&producer->control->published_sequence, sequence);
	return true;
}

extern "C" const struct pulsar_d3d11_return_control *pulsar_d3d11_return_producer_control(
	pulsar_d3d11_return_producer_t *producer)
{
	return producer ? producer->control : nullptr;
}

static bool consumer_open_ring(pulsar_d3d11_return_consumer *consumer)
{
	if (!consumer || !consumer->control || consumer->control->selected_path != PULSAR_D3D11_PATH_SHARED_TEXTURE)
		return false;
	if (consumer->control->width != PULSAR_D3D11_RETURN_WIDTH ||
		consumer->control->height != PULSAR_D3D11_RETURN_HEIGHT)
		return false;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	pulsar_d3d11_return_luid actual = {};
	HRESULT hr = create_device_for_luid(&consumer->control->adapter_luid, device, context, &actual);
	if (FAILED(hr) || actual.low != consumer->control->adapter_luid.low ||
		actual.high != consumer->control->adapter_luid.high) {
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_ADAPTER, FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND);
		return false;
	}
	consumer->device = device;
	consumer->context = context;
	for (uint32_t i = 0; i < PULSAR_D3D11_RETURN_SLOT_COUNT; ++i) {
		const uint64_t handle_value = consumer->control->slots[i].handle.value;
		if (sizeof(uintptr_t) < sizeof(uint64_t) && handle_value > UINT32_MAX) {
			set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_INTEROP,
				     HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
			return false;
		}
		HANDLE shared = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_value));
		if (!shared) {
			set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_INTEROP, E_HANDLE);
			return false;
		}
		ComPtr<ID3D11Device1> device1;
		hr = consumer->device.As(&device1);
		if (SUCCEEDED(hr))
			hr = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&consumer->textures[i]));
		CloseHandle(shared);
		consumer->control->slots[i].handle.value = 0;
		if (FAILED(hr) || FAILED(consumer->textures[i].As(&consumer->mutexes[i]))) {
			set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_INTEROP, FAILED(hr) ? hr : E_NOINTERFACE);
			return false;
		}
	}
	D3D11_TEXTURE2D_DESC desc = {};
	consumer->textures[0]->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	hr = consumer->device->CreateTexture2D(&desc, nullptr, &consumer->staging);
	if (FAILED(hr)) {
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_INTEROP, hr);
		return false;
	}
	D3D11_QUERY_DESC query_desc = {};
	query_desc.Query = D3D11_QUERY_EVENT;
	hr = consumer->device->CreateQuery(&query_desc, &consumer->query);
	if (FAILED(hr)) {
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_INTEROP, hr);
		return false;
	}
	consumer->active = true;
	InterlockedExchange((volatile LONG *)&consumer->control->consumer_ready, 1);
	return true;
}

extern "C" pulsar_d3d11_return_consumer_t *pulsar_d3d11_return_consumer_open(
	const wchar_t *name, enum pulsar_d3d11_return_lane lane, uint32_t width, uint32_t height)
{
	if (!d3d11_transport_requested() || !name || !*name ||
		width != PULSAR_D3D11_RETURN_WIDTH || height != PULSAR_D3D11_RETURN_HEIGHT)
		return nullptr;
	const std::wstring map_name = control_name(name);
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
	if (!mapping)
		return nullptr;
	auto *consumer = new pulsar_d3d11_return_consumer();
	consumer->mapping = mapping;
	consumer->control = static_cast<pulsar_d3d11_return_control *>(MapViewOfFile(
		mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(pulsar_d3d11_return_control)));
	if (!consumer->control || consumer->control->abi_version != PULSAR_D3D11_RETURN_ABI_VERSION ||
		consumer->control->lane != static_cast<uint32_t>(lane)) {
		if (consumer->control)
			UnmapViewOfFile(consumer->control);
		CloseHandle(mapping);
		delete consumer;
		return nullptr;
	}
	consumer->width = width;
	consumer->height = height;
	consumer->control->consumer_pid = GetCurrentProcessId();
	consumer->control->consumer_session = process_session(consumer->control->consumer_pid);
	if (!consumer_open_ring(consumer) && consumer->control->selected_path != PULSAR_D3D11_PATH_CPU) {
		UnmapViewOfFile(consumer->control);
		CloseHandle(consumer->mapping);
		delete consumer;
		return nullptr;
	}
	return consumer;
}

extern "C" void pulsar_d3d11_return_consumer_close(pulsar_d3d11_return_consumer_t *consumer)
{
	if (!consumer)
		return;
	if (consumer->control)
		InterlockedExchange((volatile LONG *)&consumer->control->consumer_ready, 0);
	consumer->active = false;
	if (consumer->control)
		UnmapViewOfFile(consumer->control);
	if (consumer->mapping)
		CloseHandle(consumer->mapping);
	delete consumer;
}

extern "C" bool pulsar_d3d11_return_consumer_read(pulsar_d3d11_return_consumer_t *consumer, uint8_t *dst,
								 uint64_t *timestamp,
								 struct video_queue_frame_metadata *metadata)
{
	if (!d3d11_transport_requested() || !consumer || !dst || !consumer->control)
		return false;
	if (!consumer->active && !consumer_open_ring(consumer))
		return false;
	if (consumer->control->selected_path != PULSAR_D3D11_PATH_SHARED_TEXTURE)
		return false;
	const uint64_t published = load_u64((volatile uint64_t *)&consumer->control->published_sequence);
	if (!published)
		return false;
	const uint64_t consumed = load_u64((volatile uint64_t *)&consumer->control->consumed_sequence);
	if (published > consumed + 1)
		consumer->control->gap_count += published - consumed - 1;
	const uint32_t index = static_cast<uint32_t>((published - 1) % PULSAR_D3D11_RETURN_SLOT_COUNT);
	const uint64_t wait_start = now_ns();
	const HRESULT acquire = consumer->mutexes[index]->AcquireSync(1, kWaitMs);
	const uint64_t wait_end = now_ns();
	consumer->control->mutex_wait_ns = wait_end >= wait_start ? wait_end - wait_start : 0;
	if (acquire == WAIT_TIMEOUT) {
		++consumer->control->retry_count;
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_TIMEOUT, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
		return false;
	}
	if (FAILED(acquire)) {
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_DEVICE_REMOVED, acquire);
		return false;
	}
	auto &slot = consumer->control->slots[index];
	if (slot.sequence != published || slot.epoch != consumer->control->epoch) {
		consumer->mutexes[index]->ReleaseSync(0);
		++consumer->control->retry_count;
		++consumer->control->torn_count;
		return false;
	}
	const uint64_t copy_start = now_ns();
	consumer->context->CopyResource(consumer->staging.Get(), consumer->textures[index].Get());
	consumer->context->End(consumer->query.Get());
	consumer->context->Flush();
	const uint64_t deadline = now_ns() + static_cast<uint64_t>(kWaitMs) * 1000000ULL;
	while (consumer->context->GetData(consumer->query.Get(), nullptr, 0, 0) == S_FALSE && now_ns() < deadline)
		SwitchToThread();
	if (consumer->context->GetData(consumer->query.Get(), nullptr, 0, 0) != S_OK) {
		consumer->mutexes[index]->ReleaseSync(0);
		++consumer->control->retry_count;
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_TIMEOUT, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
		return false;
	}
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = consumer->context->Map(consumer->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		consumer->mutexes[index]->ReleaseSync(0);
		set_fallback(consumer->control, PULSAR_D3D11_FALLBACK_DEVICE_REMOVED, hr);
		return false;
	}
	for (uint32_t row = 0; row < consumer->height; ++row)
		memcpy(dst + static_cast<size_t>(row) * consumer->width,
			static_cast<uint8_t *>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, consumer->width);
	uint8_t *dst_uv = dst + static_cast<size_t>(consumer->width) * consumer->height;
	uint8_t *src_uv = static_cast<uint8_t *>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * consumer->height;
	for (uint32_t row = 0; row < consumer->height / 2; ++row)
		memcpy(dst_uv + static_cast<size_t>(row) * consumer->width, src_uv + static_cast<size_t>(row) * mapped.RowPitch,
		       consumer->width);
	consumer->context->Unmap(consumer->staging.Get(), 0);
	consumer->mutexes[index]->ReleaseSync(0);
	const uint64_t copy_end = now_ns();
	consumer->control->readback_ns = copy_end >= copy_start ? copy_end - copy_start : 0;
	const uint64_t frame_clock = slot.pts_ns ? slot.pts_ns : slot.timestamp;
	consumer->control->frame_age_ns = frame_clock && copy_end >= frame_clock ? copy_end - frame_clock : 0;
	if (timestamp)
		*timestamp = slot.timestamp;
	if (metadata) {
		memset(metadata, 0, sizeof(*metadata));
		metadata->timestamp = slot.timestamp;
		metadata->frame_id = slot.frame_id;
		metadata->pts_ns = slot.pts_ns;
		metadata->server_seq = slot.server_seq;
		metadata->program_revision = slot.program_revision;
		metadata->preview_revision = slot.preview_revision;
		metadata->role_map_revision = slot.role_map_revision;
		metadata->valid = slot.valid;
		memcpy(metadata->runtime_instance_id, slot.runtime_instance_id, sizeof(metadata->runtime_instance_id));
		memcpy(metadata->command_id, slot.command_id, sizeof(metadata->command_id));
		memcpy(metadata->intent_id, slot.intent_id, sizeof(metadata->intent_id));
		memcpy(metadata->take_command_id, slot.take_command_id, sizeof(metadata->take_command_id));
	}
	store_u64((volatile uint64_t *)&consumer->control->consumed_sequence, published);
	return true;
}

extern "C" const struct pulsar_d3d11_return_control *pulsar_d3d11_return_consumer_control(
	pulsar_d3d11_return_consumer_t *consumer)
{
	return consumer ? consumer->control : nullptr;
}
