#include "d3d11-return-auth.h"

#include <bcrypt.h>
#include <string.h>

static bool auth_hash(const uint8_t *key, size_t key_size, const void *payload, size_t payload_size,
			      uint8_t output[PULSAR_RETURN_AUTH_NONCE_BYTES])
{
	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	PUCHAR object = NULL;
	DWORD object_size = 0;
	DWORD result_size = 0;
	bool ok = false;

	if (!key || key_size == 0 || !payload || !payload_size || !output)
		return false;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL,
					       BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
		goto done;
	if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
				      sizeof(object_size), &result_size, 0) < 0 || !object_size)
		goto done;
	object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_size);
	if (!object)
		goto done;
	if (BCryptCreateHash(algorithm, &hash, object, object_size, (PUCHAR)key,
				     (ULONG)key_size, 0) < 0)
		goto done;
	if (BCryptHashData(hash, (PUCHAR)payload, (ULONG)payload_size, 0) < 0)
		goto done;
	if (BCryptFinishHash(hash, output, PULSAR_RETURN_AUTH_NONCE_BYTES, 0) < 0)
		goto done;
	ok = true;

done:
	if (hash)
		BCryptDestroyHash(hash);
	if (object)
		HeapFree(GetProcessHeap(), 0, object);
	if (algorithm)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	return ok;
}

bool pulsar_return_auth_random(void *destination, size_t size)
{
	return destination && size && BCryptGenRandom(NULL, (PUCHAR)destination, (ULONG)size,
							BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool pulsar_return_auth_write(HANDLE pipe, const void *data, size_t size)
{
	const uint8_t *cursor = (const uint8_t *)data;
	if (!pipe || pipe == INVALID_HANDLE_VALUE || !data || !size)
		return false;
	while (size) {
		DWORD written = 0;
		if (!WriteFile(pipe, cursor, (DWORD)(size > UINT32_MAX ? UINT32_MAX : size), &written, NULL) || !written)
			return false;
		cursor += written;
		size -= written;
	}
	return true;
}

bool pulsar_return_auth_read(HANDLE pipe, void *data, size_t size)
{
	uint8_t *cursor = (uint8_t *)data;
	if (!pipe || pipe == INVALID_HANDLE_VALUE || !data || !size)
		return false;
	while (size) {
		DWORD read_size = 0;
		if (!ReadFile(pipe, cursor, (DWORD)(size > UINT32_MAX ? UINT32_MAX : size), &read_size, NULL) || !read_size)
			return false;
		cursor += read_size;
		size -= read_size;
	}
	return true;
}

bool pulsar_return_auth_make_proof(const uint8_t capability[PULSAR_RETURN_AUTH_CAPABILITY_BYTES],
					   const struct pulsar_return_auth_hello *hello,
					   const struct pulsar_return_auth_challenge *challenge,
					   uint8_t mac[PULSAR_RETURN_AUTH_NONCE_BYTES])
{
	struct {
		uint32_t magic;
		uint32_t version;
		uint32_t lane;
		uint8_t generation[PULSAR_RETURN_AUTH_GENERATION_BYTES];
		uint8_t hello_nonce[PULSAR_RETURN_AUTH_NONCE_BYTES];
		uint8_t challenge_nonce[PULSAR_RETURN_AUTH_NONCE_BYTES];
	} payload = {0};
	if (!capability || !hello || !challenge || !mac || hello->lane != challenge->lane ||
		!pulsar_return_auth_equal(hello->generation, challenge->generation,
					  PULSAR_RETURN_AUTH_GENERATION_BYTES))
		return false;
	payload.magic = PULSAR_RETURN_AUTH_MAGIC;
	payload.version = PULSAR_RETURN_AUTH_VERSION;
	payload.lane = hello->lane;
	memcpy(payload.generation, hello->generation, sizeof(payload.generation));
	memcpy(payload.hello_nonce, hello->nonce, sizeof(payload.hello_nonce));
	memcpy(payload.challenge_nonce, challenge->nonce, sizeof(payload.challenge_nonce));
	return auth_hash(capability, PULSAR_RETURN_AUTH_CAPABILITY_BYTES, &payload, sizeof(payload), mac);
}

bool pulsar_return_auth_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
	uint8_t difference = 0;
	if (!left || !right)
		return false;
	for (size_t i = 0; i < size; ++i)
		difference |= left[i] ^ right[i];
	return difference == 0;
}
