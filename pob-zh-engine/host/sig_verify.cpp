#include "sig_verify.h"

#include "hash_sha256.h"
#include "update_pubkeys.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

// BCRYPT_ECCKEY_BLOB 的 dwMagic。winnt 的標頭有給名字,但寫死數值再加註解,
// 是因為這個常數同時決定了「曲線」與「公/私鑰」,看到數字才知道是不是 P-256。
constexpr ULONG kEcdsaPublicP256Magic = 0x31534345; // 'ECS1' = BCRYPT_ECDSA_PUBLIC_P256_MAGIC
constexpr size_t kCoordBytes = 32;                  // P-256:X、Y、r、s 各 32 bytes
constexpr size_t kPubKeyBytes = kCoordBytes * 2;    // 64
constexpr size_t kSigBytes = kCoordBytes * 2;       // 64

std::string trim_ascii_ws(const std::string& s)
{
	size_t b = 0, e = s.size();
	auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (b < e && ws(s[b])) b++;
	while (e > b && ws(s[e - 1])) e--;
	return s.substr(b, e - b);
}

// 嚴格:必須剛好是 expectBytes*2 個十六進位字元,沒有 0x 前綴、沒有分隔符。
// 寬鬆的解析器會把「簽章檔被截斷」解讀成一個比較短的合法簽章,那正是不該
// 容忍的那種輸入。
bool hex_to_bytes(const std::string& hexIn, size_t expectBytes,
                  std::vector<unsigned char>* out, std::string* err)
{
	const std::string hex = trim_ascii_ws(hexIn);
	if (hex.size() != expectBytes * 2) {
		if (err) *err = "hex length " + std::to_string(hex.size()) +
		                ", expected " + std::to_string(expectBytes * 2);
		return false;
	}
	auto nib = [](char c, int* v) {
		if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
		if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
		if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
		return false;
	};
	out->assign(expectBytes, 0);
	for (size_t i = 0; i < expectBytes; i++) {
		int hi = 0, lo = 0;
		if (!nib(hex[i * 2], &hi) || !nib(hex[i * 2 + 1], &lo)) {
			if (err) *err = "non-hex character at offset " + std::to_string(i * 2);
			out->clear();
			return false;
		}
		(*out)[i] = (unsigned char)((hi << 4) | lo);
	}
	return true;
}

std::string bytes_to_hex(const unsigned char* p, size_t n)
{
	static const char* hex = "0123456789abcdef";
	std::string s(n * 2, '0');
	for (size_t i = 0; i < n; i++) {
		s[i * 2] = hex[p[i] >> 4];
		s[i * 2 + 1] = hex[p[i] & 0xF];
	}
	return s;
}

// RAII,因為下面每個函式都有好幾條提早 return 的路徑。
struct AlgHandle {
	BCRYPT_ALG_HANDLE h = nullptr;
	~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct KeyHandle {
	BCRYPT_KEY_HANDLE h = nullptr;
	~KeyHandle() { if (h) BCryptDestroyKey(h); }
};

void attach_parent_console()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
}

bool read_all_bytes(const std::wstring& path, std::vector<unsigned char>* out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz{};
	bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart >= 0 && sz.QuadPart < (1LL << 28);
	if (ok) {
		out->resize((size_t)sz.QuadPart);
		DWORD got = 0;
		ok = out->empty() ||
		     (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
	}
	CloseHandle(h);
	if (!ok) out->clear();
	return ok;
}

std::string narrow_path(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)(n > 0 ? n : 0), '\0');
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

} // namespace

bool VerifyDetachedSignature(const void* data, size_t size, const std::string& sigHex,
                             const std::string& pubKeyHex, std::string* err)
{
	std::vector<unsigned char> sig, pub;
	if (!hex_to_bytes(sigHex, kSigBytes, &sig, err)) {
		if (err) *err = "signature: " + *err;
		return false;
	}
	if (!hex_to_bytes(pubKeyHex, kPubKeyBytes, &pub, err)) {
		if (err) *err = "public key: " + *err;
		return false;
	}

	unsigned char digest[32] = {};
	if (!Sha256Raw(data, size, digest)) {
		if (err) *err = "sha256 failed";
		return false;
	}

	AlgHandle alg;
	if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) {
		if (err) *err = "BCryptOpenAlgorithmProvider(ECDSA_P256) failed";
		return false;
	}

	// BCRYPT_ECCKEY_BLOB { ULONG dwMagic; ULONG cbKey; } 後面接 X[32] Y[32]。
	std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + kPubKeyBytes);
	auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
	hdr->dwMagic = kEcdsaPublicP256Magic;
	hdr->cbKey = (ULONG)kCoordBytes;
	memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pub.data(), kPubKeyBytes);

	KeyHandle key;
	if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.h,
	                        blob.data(), (ULONG)blob.size(), 0) != 0) {
		// 走到這裡通常代表公鑰不在曲線上(打錯字、抄漏一個字元)。
		if (err) *err = "BCryptImportKeyPair failed (public key not a valid P-256 point?)";
		return false;
	}

	// ECDSA:padding info 必須是 nullptr,簽章是 r||s 原始格式。
	const NTSTATUS st = BCryptVerifySignature(key.h, nullptr, digest, sizeof(digest),
	                                          sig.data(), (ULONG)sig.size(), 0);
	if (st != 0) {
		if (err) *err = "signature does not verify";
		return false;
	}
	return true;
}

bool VerifyReleaseSignatureWithKeys(const void* data, size_t size, const std::string& sigHex,
                                   const char* const* keysHex, int keyCount,
                                   int* keyIndex, std::string* err)
{
	if (keyIndex) *keyIndex = -1;
	// 空清單代表 update_pubkeys.h 沒被產生過。那不是「沒有金鑰所以放行」,
	// 是建置壞了 —— 不能靜默地退化成不驗證。
	if (!keysHex || keyCount <= 0) {
		if (err) *err = "no public keys compiled in";
		return false;
	}
	std::string last;
	for (int i = 0; i < keyCount; i++) {
		std::string e;
		if (VerifyDetachedSignature(data, size, sigHex, keysHex[i], &e)) {
			if (keyIndex) *keyIndex = i;
			return true;
		}
		last = e;
	}
	// 只回報最後一把的原因就夠了:每一把的失敗都是同一句「驗不過」,而金鑰
	// 本身有問題(長度、不在曲線上)在建置期就會被自檢擋下。
	if (err) *err = "no compiled-in key verifies this signature (" + last + ")";
	return false;
}

bool VerifyReleaseSignature(const void* data, size_t size, const std::string& sigHex,
                            int* keyIndex, std::string* err)
{
	// NOT logged here, deliberately. This is a primitive: --app-update-selftest
	// calls it with deliberately bad signatures to prove the rejection works, and
	// logging at this level put two "release signature rejected" lines into the
	// failure log on every clean test run. A log that reports healthy behaviour as
	// a problem stops being read.
	//
	// The real refusals are logged one level up, where a failure means an update
	// was actually turned away: app_update.cpp routes trust failures through
	// PobLog::Error("sig", ...) with the context of which line and which release.
	return VerifyReleaseSignatureWithKeys(data, size, sigHex, kUpdatePublicKeysHex,
	                                      kUpdatePublicKeyCount, keyIndex, err);
}

bool SignWithEphemeralKeyForTest(const void* data, size_t size,
                                 std::string* pubKeyHex, std::string* sigHex)
{
	if (!pubKeyHex || !sigHex) return false;
	pubKeyHex->clear();
	sigHex->clear();

	unsigned char digest[32] = {};
	if (!Sha256Raw(data, size, digest)) return false;

	AlgHandle alg;
	if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
		return false;

	KeyHandle key;
	if (BCryptGenerateKeyPair(alg.h, &key.h, 256, 0) != 0) return false;
	if (BCryptFinalizeKeyPair(key.h, 0) != 0) return false;

	DWORD need = 0;
	if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &need, 0) != 0)
		return false;
	std::vector<unsigned char> blob(need);
	if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), need, &need, 0) != 0)
		return false;
	if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + kPubKeyBytes) return false;
	const auto* hdr = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
	if (hdr->cbKey != kCoordBytes) return false;
	*pubKeyHex = bytes_to_hex(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kPubKeyBytes);

	unsigned char sig[kSigBytes] = {};
	DWORD sigLen = 0;
	if (BCryptSignHash(key.h, nullptr, digest, sizeof(digest), sig, (ULONG)sizeof(sig),
	                   &sigLen, 0) != 0)
		return false;
	if (sigLen != kSigBytes) return false;
	*sigHex = bytes_to_hex(sig, kSigBytes);
	return true;
}

int RunVerifyManifestCli(const std::wstring& manifestPath, const std::wstring& sigPath)
{
	attach_parent_console();

	if (manifestPath.empty() || sigPath.empty()) {
		printf("FAIL: usage: pob-zh.exe --verify-manifest <manifest.json> <manifest.json.sig>\n");
		return 2;
	}
	std::vector<unsigned char> body, sigRaw;
	if (!read_all_bytes(manifestPath, &body)) {
		printf("FAIL: cannot read %s\n", narrow_path(manifestPath).c_str());
		return 1;
	}
	if (!read_all_bytes(sigPath, &sigRaw)) {
		printf("FAIL: cannot read %s\n", narrow_path(sigPath).c_str());
		return 1;
	}
	// 空的 manifest 驗得過任何一把金鑰所簽的空 manifest,但那不是有用的東西 ——
	// 打包腳本傳錯路徑(拿到 0 bytes)在這裡就要停,不要一路 PASS 到發佈。
	if (body.empty()) {
		printf("FAIL: manifest is empty\n");
		return 1;
	}

	const std::string sigHex(reinterpret_cast<const char*>(sigRaw.data()), sigRaw.size());
	int keyIndex = -1;
	std::string err;
	if (!VerifyReleaseSignature(body.data(), body.size(), sigHex, &keyIndex, &err)) {
		printf("FAIL: %s\n", err.c_str());
		return 1;
	}
	printf("OK: manifest signature verified with compiled-in key #%d (%zu bytes signed)\n",
	       keyIndex, body.size());
	return 0;
}
