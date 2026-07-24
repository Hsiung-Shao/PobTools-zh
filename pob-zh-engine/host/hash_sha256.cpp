#include "hash_sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")

bool Sha256Hex(const void* data, size_t size, std::string* hexLower)
{
	if (!hexLower) return false;
	hexLower->clear();

	BCRYPT_ALG_HANDLE hAlg = nullptr;
	if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
		return false;

	bool ok = false;
	DWORD objLen = 0, cb = 0;
	UCHAR digest[32] = {};
	std::vector<UCHAR> obj;
	BCRYPT_HASH_HANDLE hHash = nullptr;
	if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) == 0) {
		obj.resize(objLen);
		if (BCryptCreateHash(hAlg, &hHash, obj.data(), objLen, nullptr, 0, 0) == 0) {
			// 25MB assets hash in one call; sizes here never approach ULONG range.
			if (BCryptHashData(hHash, (PUCHAR)data, (ULONG)size, 0) == 0 &&
			    BCryptFinishHash(hHash, digest, sizeof(digest), 0) == 0)
				ok = true;
			BCryptDestroyHash(hHash);
		}
	}
	BCryptCloseAlgorithmProvider(hAlg, 0);
	if (!ok) return false;

	static const char* hex = "0123456789abcdef";
	hexLower->resize(64);
	for (int i = 0; i < 32; i++) {
		(*hexLower)[i * 2] = hex[digest[i] >> 4];
		(*hexLower)[i * 2 + 1] = hex[digest[i] & 0xF];
	}
	return true;
}
