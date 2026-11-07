// Optional GitHub API authentication, shared by the launcher and CLI workers.
#pragma once

#include <string>

// Every message this module can put in front of the user. A macro rather than an
// array so app_update.cpp can splice the literals straight into kAppUpdateGlyphSeed:
// the glyph seed stays one compile-time string and cannot drift from this list.
#define PT_GITHUB_ERRORS(X) \
	X(kInvalid,       u8"GitHub Token 無效或已過期，請到設定的「網路與更新」更換或清除 Token") \
	X(kLimited,       u8"GitHub 暫時限制查詢次數，請稍後再試；可到設定的「網路與更新」儲存 GitHub Token") \
	X(kLimitedToken,  u8"GitHub 暫時限制查詢次數，請稍後再試") \
	X(kForbidden,     u8"GitHub 拒絕存取，請檢查 Token 權限或稍後再試") \
	X(kForbiddenAnon, u8"GitHub 拒絕存取，請稍後再試") \
	X(kReadFailed,    u8"無法讀取已儲存的 GitHub Token，請重新儲存") \
	X(kDecryptFailed, u8"無法解密已儲存的 GitHub Token，請在此 Windows 使用者下重新儲存") \
	X(kStoredInvalid, u8"已儲存的 GitHub Token 格式無效，請重新儲存") \
	X(kFormatInvalid, u8"Token 格式無效，請只貼上 ghp_ 或 github_pat_ 開頭的 Personal access token") \
	X(kClearFailed,   u8"無法清除 GitHub Token，請檢查資料夾寫入權限") \
	X(kEncryptFailed, u8"無法加密 GitHub Token，未儲存任何變更") \
	X(kSaveFailed,    u8"無法儲存 GitHub Token，請檢查資料夾寫入權限")

namespace GitHubToken {

// Trims pasted whitespace; accepts only classic (ghp_) and fine-grained
// (github_pat_) personal access tokens, so a password or another service's key
// pasted by mistake is never sent anywhere. Empty is valid and means anonymous.
// Never includes input in errors.
bool Normalize(const std::string& input, std::string& token);

// <exeDir>PobTools\github_token.bin, encrypted for the current Windows user.
// Save is atomic; empty deletes the saved credential. Failure keeps the old one.
std::string Load(const std::wstring& exeDir, std::string* err = nullptr);
bool Save(const std::wstring& exeDir, const std::string& input, std::string* err = nullptr);

// Zeroes the characters before clearing, for buffers that held a token.
void Wipe(std::string& value);

// Thread-safe: settings changes affect the next request, even on an existing client.
bool Set(const std::string& input);
bool IsApiHost(const std::wstring& host);
// Empty unless host is api.github.com and a token is set.
std::wstring RequestHeaders(const std::wstring& host);

// Only call for api.github.com. Returns the user-facing message for the statuses
// worth naming (401, rate limits, 403), or "" when the caller should keep its own
// "HTTP <code>: <path>" wording. usedToken picks the wording: a user who already
// saved a token is not told to save one, an anonymous user is not told to check
// a token they do not have. Raw response bodies are never returned or logged.
std::string ApiError(int status, const std::string& remaining,
                     const std::string& retryAfter, const std::string& body, bool usedToken);

// GitHub answered 401 to the current token (expired or revoked). Requests fall
// back to anonymous; the settings page reads Rejected() to say so. Cleared
// whenever the token changes.
void NoteRejected();
bool Rejected();

// Load <exeDir>PobTools\github_token.bin and keep watching it: RequestHeaders
// re-reads the file when its size or write time changed, so a tool process that
// was already running follows a token the launcher saved or cleared since.
void Init(const std::wstring& exeDir);
extern const char* const ErrorGlyphSeed;

} // namespace GitHubToken
