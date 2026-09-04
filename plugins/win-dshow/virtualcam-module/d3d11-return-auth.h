#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define PULSAR_RETURN_AUTH_MAGIC 0x41525450U /* "PTRA" */
#define PULSAR_RETURN_AUTH_VERSION 1U
#define PULSAR_RETURN_AUTH_CAPABILITY_BYTES 32U
#define PULSAR_RETURN_AUTH_GENERATION_BYTES 16U
#define PULSAR_RETURN_AUTH_NONCE_BYTES 32U
#define PULSAR_RETURN_AUTH_QUEUE_NAME_CAPACITY 256U
#define PULSAR_RETURN_AUTH_FRAME_MAGIC 0x46525450U /* "PTRF" */
#define PULSAR_RETURN_AUTH_FRAME_METADATA_BYTES 576U

#pragma pack(push, 1)
struct pulsar_return_auth_bootstrap {
	uint32_t magic;
	uint32_t version;
	uint32_t lane;
	uint32_t width;
	uint32_t height;
	uint64_t interval;
	uint8_t generation[PULSAR_RETURN_AUTH_GENERATION_BYTES];
	uint8_t capability[PULSAR_RETURN_AUTH_CAPABILITY_BYTES];
	wchar_t queue_name[PULSAR_RETURN_AUTH_QUEUE_NAME_CAPACITY];
};

struct pulsar_return_auth_hello {
	uint32_t magic;
	uint32_t version;
	uint32_t lane;
	uint8_t generation[PULSAR_RETURN_AUTH_GENERATION_BYTES];
	uint8_t nonce[PULSAR_RETURN_AUTH_NONCE_BYTES];
};

struct pulsar_return_auth_challenge {
	uint32_t magic;
	uint32_t version;
	uint32_t lane;
	uint8_t generation[PULSAR_RETURN_AUTH_GENERATION_BYTES];
	uint8_t nonce[PULSAR_RETURN_AUTH_NONCE_BYTES];
};

struct pulsar_return_auth_proof {
	uint32_t magic;
	uint32_t version;
	uint32_t lane;
	uint8_t generation[PULSAR_RETURN_AUTH_GENERATION_BYTES];
	uint8_t mac[PULSAR_RETURN_AUTH_NONCE_BYTES];
};

struct pulsar_return_auth_frame {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t payload_size;
	uint32_t reserved;
	uint64_t timestamp;
	uint8_t metadata[PULSAR_RETURN_AUTH_FRAME_METADATA_BYTES];
};
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

bool pulsar_return_auth_random(void *destination, size_t size);
bool pulsar_return_auth_write(HANDLE pipe, const void *data, size_t size);
bool pulsar_return_auth_read(HANDLE pipe, void *data, size_t size);
bool pulsar_return_auth_make_proof(const uint8_t capability[PULSAR_RETURN_AUTH_CAPABILITY_BYTES],
					   const struct pulsar_return_auth_hello *hello,
					   const struct pulsar_return_auth_challenge *challenge,
					   uint8_t mac[PULSAR_RETURN_AUTH_NONCE_BYTES]);
bool pulsar_return_auth_equal(const uint8_t *left, const uint8_t *right, size_t size);

#ifdef __cplusplus
}
#endif
