#include "github_token.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <mutex>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace GitHubToken {
namespace {

std::mutex tokenMutex;
std::string currentToken;
const wchar_t kEntropy[] = L"PobTools.GitHubToken";
constexpr size_t kMaxTokenSize = 4096;

#define PT_GITHUB_ERRORS(X) \
    X(kInvalid, u8"GitHub Token 無效或已過期，請到設定的「網路與更新」更換或清除 Token") \
    X(kLimited, u8"GitHub 暫時限制查詢次數，請稍後再試；可到設定的「網路與更新」儲存 GitHub Token") \
    X(kForbidden, u8"GitHub 拒絕存取，請檢查 Token 權限或稍後再試") \
    X(kReadFailed, u8"無法讀取已儲存的 GitHub Token，請重新儲存") \
    X(kDecryptFailed, u8"無法解密已儲存的 GitHub Token，請在此 Windows 使用者下重新儲存") \
    X(kStoredInvalid, u8"已儲存的 GitHub Token 格式無效，請重新儲存") \
    X(kFormatInvalid, u8"Token 格式無效，請只貼上 GitHub Personal access token") \
    X(kClearFailed, u8"無法清除 GitHub Token，請檢查資料夾寫入權限") \
    X(kEncryptFailed, u8"無法加密 GitHub Token，未儲存任何變更") \
    X(kSaveFailed, u8"無法儲存 GitHub Token，請檢查資料夾寫入權限")
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

void Wipe(std::string& value)
{
    if (!value.empty()) SecureZeroMemory(&value[0], value.size());
    value.clear();
}

} // namespace

#define PT_SEED(name, text) text
const char* const ErrorGlyphSeed = PT_GITHUB_ERRORS(PT_SEED);
#undef PT_SEED
#undef PT_GITHUB_ERRORS

bool Normalize(const std::string& input, std::string& token)
{
    const auto whitespace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0, end = input.size();
    while (begin < end && whitespace((unsigned char)input[begin])) ++begin;
    while (end > begin && whitespace((unsigned char)input[end - 1])) --end;
    if (end - begin > kMaxTokenSize) { token.clear(); return false; }
    for (size_t i = begin; i < end; ++i) {
        const unsigned char c = (unsigned char)input[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) { token.clear(); return false; }
    }
    token = input.substr(begin, end - begin);
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
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 65536;
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        ok = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(file);
    DATA_BLOB in{ (DWORD)bytes.size(), bytes.data() };
    DATA_BLOB entropy{ sizeof(kEntropy), (BYTE*)kEntropy };
    DATA_BLOB out{};
    if (!ok || !CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
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

bool Set(const std::string& input)
{
    std::string token;
    if (!Normalize(input, token)) return false;
    std::lock_guard<std::mutex> lock(tokenMutex);
    Wipe(currentToken);
    currentToken.swap(token);
    return true;
}

bool IsApiHost(const std::wstring& host)
{
    return host.size() == 14 && _wcsicmp(host.c_str(), L"api.github.com") == 0;
}

std::wstring RequestHeaders(const std::wstring& host)
{
    if (!IsApiHost(host)) return {};
    std::lock_guard<std::mutex> lock(tokenMutex);
    if (currentToken.empty()) return {};
    return L"Authorization: Bearer " + std::wstring(currentToken.begin(), currentToken.end()) + L"\r\n";
}

std::string ApiError(int status, const std::string& remaining,
                     const std::string& retryAfter, const std::string& body)
{
    if (status == 401) return kInvalid;
    if (status == 429 || (status == 403 && (remaining == "0" || !retryAfter.empty() ||
        body.find("rate limit") != std::string::npos))) return kLimited;
    if (status == 403) return kForbidden;
    return "HTTP " + std::to_string(status);
}

} // namespace GitHubToken
