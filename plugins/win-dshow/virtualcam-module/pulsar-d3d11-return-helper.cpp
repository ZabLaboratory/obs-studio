#include "d3d11-return-auth.h"
#include "d3d11-return-transport.hpp"

#include "../../../shared/obs-shared-memory-queue/shared-memory-queue.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

static bool parse_handle(const wchar_t *value, HANDLE *handle)
{
	if (!value || !handle)
		return false;
	wchar_t *end = nullptr;
	const unsigned long long parsed = wcstoull(value, &end, 0);
	if (end == value || *end || !parsed)
		return false;
	*handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parsed));
	return true;
}

static const wchar_t *argument_value(int argc, wchar_t **argv, const wchar_t *prefix)
{
	const size_t length = wcslen(prefix);
	for (int i = 1; i < argc; ++i) {
		if (wcsncmp(argv[i], prefix, length) == 0)
			return argv[i] + length;
	}
	return nullptr;
}

static bool validate_bootstrap(const pulsar_return_auth_bootstrap &bootstrap)
{
	return bootstrap.magic == PULSAR_RETURN_AUTH_MAGIC && bootstrap.version == PULSAR_RETURN_AUTH_VERSION &&
		(bootstrap.lane == PULSAR_D3D11_PROGRAM_RETURN || bootstrap.lane == PULSAR_D3D11_PREVIEW_RETURN) &&
		bootstrap.width == PULSAR_D3D11_RETURN_WIDTH && bootstrap.height == PULSAR_D3D11_RETURN_HEIGHT &&
		bootstrap.interval && bootstrap.queue_name[0] != L'\0';
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	HANDLE read_pipe = nullptr;
	HANDLE write_pipe = nullptr;
	if (!parse_handle(argument_value(argc, argv, L"--read-handle="), &read_pipe) ||
		!parse_handle(argument_value(argc, argv, L"--write-handle="), &write_pipe))
		return ERROR_INVALID_PARAMETER;

	pulsar_return_auth_bootstrap bootstrap = {};
	if (!pulsar_return_auth_read(read_pipe, &bootstrap, sizeof(bootstrap)) || !validate_bootstrap(bootstrap))
		return ERROR_ACCESS_DENIED;

	pulsar_return_auth_hello hello = {};
	hello.magic = PULSAR_RETURN_AUTH_MAGIC;
	hello.version = PULSAR_RETURN_AUTH_VERSION;
	hello.lane = bootstrap.lane;
	memcpy(hello.generation, bootstrap.generation, sizeof(hello.generation));
	if (!pulsar_return_auth_random(hello.nonce, sizeof(hello.nonce)) ||
		!pulsar_return_auth_write(write_pipe, &hello, sizeof(hello)))
		return ERROR_ACCESS_DENIED;

	pulsar_return_auth_challenge challenge = {};
	if (!pulsar_return_auth_read(read_pipe, &challenge, sizeof(challenge)) ||
		challenge.magic != PULSAR_RETURN_AUTH_MAGIC || challenge.version != PULSAR_RETURN_AUTH_VERSION ||
		challenge.lane != bootstrap.lane ||
		!pulsar_return_auth_equal(challenge.generation, bootstrap.generation,
					  PULSAR_RETURN_AUTH_GENERATION_BYTES))
		return ERROR_ACCESS_DENIED;

	pulsar_return_auth_proof proof = {};
	proof.magic = PULSAR_RETURN_AUTH_MAGIC;
	proof.version = PULSAR_RETURN_AUTH_VERSION;
	proof.lane = bootstrap.lane;
	memcpy(proof.generation, bootstrap.generation, sizeof(proof.generation));
	if (!pulsar_return_auth_make_proof(bootstrap.capability, &hello, &challenge, proof.mac) ||
		!pulsar_return_auth_write(write_pipe, &proof, sizeof(proof)))
		return ERROR_ACCESS_DENIED;

	/* The helper is the only process that receives duplicated D3D11 handles.
	 * DirectShow remains a read-only CPU consumer and never enters this path. */
	_putenv_s("PULSAR_RETURN_TRANSPORT", "d3d11");
	pulsar_d3d11_return_consumer_t *consumer = pulsar_d3d11_return_consumer_open(
		bootstrap.queue_name, static_cast<enum pulsar_d3d11_return_lane>(bootstrap.lane), bootstrap.width,
		bootstrap.height);
	if (!consumer)
		return ERROR_NOT_READY;

	const size_t frame_size = static_cast<size_t>(bootstrap.width) * bootstrap.height * 3 / 2;
	std::vector<uint8_t> frame(frame_size);
	while (true) {
		struct video_queue_frame_metadata metadata = {};
		uint64_t timestamp = 0;
		if (!pulsar_d3d11_return_consumer_read(consumer, frame.data(), &timestamp, &metadata)) {
			if (WaitForSingleObject(GetCurrentProcess(), 0) != WAIT_TIMEOUT)
				break;
			Sleep(1);
			continue;
		}
		pulsar_return_auth_frame envelope = {};
		envelope.magic = PULSAR_RETURN_AUTH_FRAME_MAGIC;
		envelope.version = PULSAR_RETURN_AUTH_VERSION;
		envelope.width = bootstrap.width;
		envelope.height = bootstrap.height;
		envelope.payload_size = static_cast<uint32_t>(frame.size());
		envelope.timestamp = timestamp;
		memcpy(envelope.metadata, &metadata, sizeof(metadata));
		if (!pulsar_return_auth_write(write_pipe, &envelope, sizeof(envelope)) ||
			!pulsar_return_auth_write(write_pipe, frame.data(), frame.size()))
			break;
	}

	pulsar_d3d11_return_consumer_close(consumer);
	return ERROR_BROKEN_PIPE;
}
