#include "github_token.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace GitHubToken {
namespace {

std::mutex tokenMutex;
std::string currentToken;          // guarded by tokenMutex
std::atomic<bool> rejected{false};
unsigned tokenGeneration = 0;      // guarded by tokenMutex; bumped on every replace

// Init's watch on the saved file; all guarded by tokenMutex.
struct FileStamp {
	bool exists = false;
	unsigned long long size = 0, writeTime = 0;
	bool operator==(const FileStamp& o) const
	{ return exists == o.exists && size == o.size && writeTime == o.writeTime; }
};
std::wstring watchedDir;
FileStamp watchedStamp;
const wchar_t kEntropy[] = L"PobTools.GitHubToken";
constexpr size_t kMaxTokenSize = 4096;
constexpr DWORD kMaxFileSize = 65536;

#define PT_DEFINE(name, text) const char* const name = text;
PT_GITHUB_ERRORS(PT_DEFINE)
#undef PT_DEFINE

std::wstring Path(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\github_token.bin";
}

bool Fail(std::string* err, const char* message)
{
	if (err) *err = message;
	return false;
}

FileStamp Stamp(const std::wstring& path)
{
	FileStamp s;
	WIN32_FILE_ATTRIBUTE_DATA a{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return s;
	s.exists = true;
	s.size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	s.writeTime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) |
	              a.ftLastWriteTime.dwLowDateTime;
	return s;
}

bool StartsWith(const char* s, size_t n, const char* prefix)
{
	const size_t len = std::strlen(prefix);
	return n >= len && std::memcmp(s, prefix, len) == 0;
}

} // namespace

#define PT_SEED(name, text) text
const char* const ErrorGlyphSeed = PT_GITHUB_ERRORS(PT_SEED);
#undef PT_SEED

void Wipe(std::string& value)
{
	if (!value.empty()) SecureZeroMemory(&value[0], value.size());
	value.clear();
}

bool Normalize(const std::string& input, std::string& token)
{
	const auto whitespace = [](unsigned char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	size_t begin = 0, end = input.size();
	while (begin < end && whitespace((unsigned char)input[begin])) ++begin;
	while (end > begin && whitespace((unsigned char)input[end - 1])) --end;
	const size_t n = end - begin;
	if (n == 0) { token.clear(); return true; }
	if (n > kMaxTokenSize) { token.clear(); return false; }
	const char* s = input.data() + begin;
	if (!StartsWith(s, n, "ghp_") && !StartsWith(s, n, "github_pat_")) { token.clear(); return false; }
	for (size_t i = 0; i < n; ++i) {
		const unsigned char c = (unsigned char)s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_')) { token.clear(); return false; }
	}
	// Assign through a temporary: input and token may be the same string.
	std::string normalized(s, n);
	token.swap(normalized);
	Wipe(normalized);
	return true;
}

std::string Load(const std::wstring& exeDir, std::string* err)
{
	if (err) err->clear();
	HANDLE file = CreateFileW(Path(exeDir).c_str(), GENERIC_READ, FILE_SHARE_READ,
	                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		const DWORD code = GetLastError();
		if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
			Fail(err, kReadFailed);
		return {};
	}
	LARGE_INTEGER size{};
	std::vector<unsigned char> bytes;
	DWORD read = 0;
	bool ok = GetFileSizeEx(file, &size) != FALSE;
	if (ok && (size.QuadPart <= 0 || size.QuadPart > kMaxFileSize)) {
		// An empty or oversized file was never written by Save: it is not ours.
		CloseHandle(file);
		Fail(err, kStoredInvalid);
		return {};
	}
	if (ok) {
		bytes.resize((size_t)size.QuadPart);
		ok = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr) && read == bytes.size();
	}
	CloseHandle(file);
	if (!ok) {
		Fail(err, kReadFailed);
		return {};
	}
	DATA_BLOB in{ (DWORD)bytes.size(), bytes.data() };
	DATA_BLOB entropy{ sizeof(kEntropy), (BYTE*)kEntropy };
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
	                        CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		Fail(err, kDecryptFailed);
		return {};
	}
	std::string plain((const char*)out.pbData, out.cbData), token;
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	ok = Normalize(plain, token) && !token.empty();
	Wipe(plain);
	if (!ok) {
		Fail(err, kStoredInvalid);
		return {};
	}
	return token;
}

bool Save(const std::wstring& exeDir, const std::string& input, std::string* err)
{
	if (err) err->clear();
	std::string token;
	if (!Normalize(input, token))
		return Fail(err, kFormatInvalid);
	const std::wstring path = Path(exeDir);
	if (token.empty()) {
		if (DeleteFileW(path.c_str())) return true;
		const DWORD code = GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
		       Fail(err, kClearFailed);
	}
	DATA_BLOB in{ (DWORD)token.size(), (BYTE*)token.data() };
	DATA_BLOB entropy{ sizeof(kEntropy), (BYTE*)kEntropy };
	DATA_BLOB out{};
	const bool encrypted = CryptProtectData(&in, L"PobTools GitHub Token", &entropy,
	                                        nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != FALSE;
	Wipe(token);
	if (!encrypted) return Fail(err, kEncryptFailed);

	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	const std::wstring temp = path + L".tmp";
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD written = 0;
	bool ok = file != INVALID_HANDLE_VALUE;
	if (ok) {
		ok = WriteFile(file, out.pbData, out.cbData, &written, nullptr) && written == out.cbData;
		if (ok) ok = FlushFileBuffers(file) != FALSE;
		CloseHandle(file);
	}
	LocalFree(out.pbData);
	if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(),
	                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	if (!ok) {
		DeleteFileW(temp.c_str());
		return Fail(err, kSaveFailed);
	}
	return true;
}

// Caller holds tokenMutex. A new generation means "a token the user set again":
// whatever was rejected before is given another chance.
static void ReplaceLocked(std::string& token)
{
	Wipe(currentToken);
	currentToken.swap(token);
	Wipe(token);
	if (++tokenGeneration == 0) ++tokenGeneration; // 0 means "no token"
	rejected = false;
}

bool Set(const std::string& input)
{
	std::string token;
	if (!Normalize(input, token)) return false;
	std::lock_guard<std::mutex> lock(tokenMutex);
	ReplaceLocked(token);
	return true;
}

void Init(const std::wstring& exeDir)
{
	// Stamp before reading: if the file changes in between, the stamp is the
	// old one and the next request notices and reads again.
	const FileStamp stamp = Stamp(Path(exeDir));
	std::string token = Load(exeDir);
	std::lock_guard<std::mutex> lock(tokenMutex);
	watchedDir = exeDir;
	watchedStamp = stamp;
	ReplaceLocked(token);
}

void NoteRejected(unsigned generation)
{
	// Only the token that was actually sent: a 401 arriving after the user saved
	// a new token must not mark the new one.
	std::lock_guard<std::mutex> lock(tokenMutex);
	if (generation != 0 && generation == tokenGeneration) rejected = true;
}

bool Rejected() { return rejected; }

bool IsApiHost(const std::wstring& host)
{
	return host.size() == 14 && _wcsicmp(host.c_str(), L"api.github.com") == 0;
}

std::wstring RequestHeaders(const std::wstring& host, unsigned* generation)
{
	if (generation) *generation = 0;
	if (!IsApiHost(host)) return {};

	// Follow the saved file (Init). The file I/O and DPAPI run outside the lock,
	// so a slow disk cannot freeze the settings page waiting in Set.
	std::wstring dir;
	FileStamp seen;
	{
		std::lock_guard<std::mutex> lock(tokenMutex);
		dir = watchedDir;
		seen = watchedStamp;
	}
	if (!dir.empty()) {
		const FileStamp now = Stamp(Path(dir));
		if (!(now == seen)) {
			std::string token = Load(dir);
			std::lock_guard<std::mutex> lock(tokenMutex);
			if (watchedStamp == seen) { // nobody applied this change meanwhile
				watchedStamp = now;
				// Re-reading the same token keeps its rejection; a different one
				// starts fresh.
				if (token != currentToken) ReplaceLocked(token);
			}
			Wipe(token);
		}
	}

	std::lock_guard<std::mutex> lock(tokenMutex);
	// A rejected token is not sent again: every request would otherwise pay a
	// 401 round trip before the anonymous retry.
	if (currentToken.empty() || rejected) return {};
	if (generation) *generation = tokenGeneration;
	return L"Authorization: Bearer " + std::wstring(currentToken.begin(), currentToken.end()) + L"\r\n";
}

std::string ApiError(int status, const std::string& remaining,
                     const std::string& retryAfter, const std::string& body, bool usedToken)
{
	if (status == 401) return usedToken ? kInvalid : std::string();
	if (status == 429 || (status == 403 && (remaining == "0" || !retryAfter.empty() ||
		body.find("rate limit") != std::string::npos))) return usedToken ? kLimitedToken : kLimited;
	if (status == 403) return usedToken ? kForbidden : kForbiddenAnon;
	return {};
}

} // namespace GitHubToken
