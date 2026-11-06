// Standalone Windows regression test. Uses real DPAPI and a fake WinHTTP
// transport: no network, real tokens, installed POB, or UI runtime required.
// Compile with /D_WINHTTP_INTERNAL_ so WinHTTP calls resolve to these fixtures.
// From a Visual Studio Developer Command Prompt, in pob-zh-engine:
//   mkdir build\github-token-tests
//   cl /std:c++17 /EHsc /utf-8 /MT /D_WINHTTP_INTERNAL_ /Fo:build\github-token-tests\ /Fe:build\github-token-tests\github_token_test.exe tests\github_token_test.cpp host\github_token.cpp host\http_client.cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "../host/github_token.h"
#include "../host/http_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* name)
{
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
struct Reply {
    DWORD status = 200;
    std::string body = "{}";
    std::wstring remaining, retryAfter, sentHeaders;
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    size_t offset = 0;
    bool rejectPolicy = false, sent = false;
} reply;

void Reset(DWORD status = 200, const std::string& body = "{}")
{
    reply = Reply{};
    reply.status = status;
    reply.body = body;
}

std::string Read(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buffer[65536];
    DWORD read = 0;
    ReadFile(f, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(f);
    return std::string(buffer, read);
}
} // namespace

namespace PobLog { void Error(const char*, const std::string&) {} }

extern "C" {
HINTERNET WINAPI WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) { return (HINTERNET)1; }
HINTERNET WINAPI WinHttpConnect(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) { return (HINTERNET)2; }
HINTERNET WINAPI WinHttpOpenRequest(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD)
{
    reply.offset = 0;
    return (HINTERNET)3;
}
BOOL WINAPI WinHttpCloseHandle(HINTERNET) { return TRUE; }
BOOL WINAPI WinHttpSetTimeouts(HINTERNET, int, int, int, int) { return TRUE; }
BOOL WINAPI WinHttpSetOption(HINTERNET, DWORD option, LPVOID value, DWORD)
{
    if (option == WINHTTP_OPTION_REDIRECT_POLICY) {
        if (reply.rejectPolicy) return FALSE;
        reply.redirectPolicy = *(DWORD*)value;
    }
    return TRUE;
}
BOOL WINAPI WinHttpSendRequest(HINTERNET, LPCWSTR headers, DWORD size, LPVOID, DWORD, DWORD, DWORD_PTR)
{
    reply.sent = true;
    reply.sentHeaders = headers ? std::wstring(headers, size) : L"";
    return TRUE;
}
BOOL WINAPI WinHttpReceiveResponse(HINTERNET, LPVOID) { return TRUE; }
BOOL WINAPI WinHttpQueryHeaders(HINTERNET, DWORD query, LPCWSTR name, LPVOID buffer, LPDWORD size, LPDWORD)
{
    if (query == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
        *(DWORD*)buffer = reply.status;
        return TRUE;
    }
    if (query != WINHTTP_QUERY_CUSTOM) return FALSE;
    const std::wstring value = _wcsicmp(name, L"Retry-After") == 0 ? reply.retryAfter : reply.remaining;
    if (value.empty() || *size < (value.size() + 1) * sizeof(wchar_t)) return FALSE;
    std::memcpy(buffer, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
    *size = (DWORD)(value.size() * sizeof(wchar_t));
    return TRUE;
}
BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET, LPDWORD size)
{
    *size = (DWORD)(reply.body.size() - reply.offset);
    return TRUE;
}
BOOL WINAPI WinHttpReadData(HINTERNET, LPVOID buffer, DWORD size, LPDWORD read)
{
    *read = (DWORD)(std::min)((size_t)size, reply.body.size() - reply.offset);
    std::memcpy(buffer, reply.body.data() + reply.offset, *read);
    reply.offset += *read;
    return TRUE;
}
} // extern "C"

int main()
{
    using namespace GitHubToken;
    const std::string fake = "github_pat_TEST_ONLY_12345";
    std::string token, err, body;
    Check(Normalize(" \r\n" + fake + "\t", token) && token == fake, "trim pasted whitespace");
    Check(Normalize(token, token) && token == fake, "normalization supports the UI's in-place save");
    Check(Normalize(" \r\n", token) && token.empty(), "empty means anonymous");
    for (const std::string& bad : std::vector<std::string>{"ghp_x\r\nInjected: value", "Bearer ghp_x", "ghp_x y", "\xC3\xA9", std::string(4097, 'x')})
        Check(!Normalize(bad, token) && token.empty(), "reject header injection, prefixes and invalid input");

    Check(Set(fake), "set fake token");
    Check(RequestHeaders(L"API.GITHUB.COM") == L"Authorization: Bearer github_pat_TEST_ONLY_12345\r\n", "API Bearer header");
    for (const wchar_t* host : {L"github.com", L"raw.githubusercontent.com", L"objects.githubusercontent.com",
                              L"api.github.com.evil.example", L"evil-api.github.com", L"poe.ninja"})
        Check(RequestHeaders(host).empty(), "no credential on downloads, lookalikes or unrelated services");
    Check(!Set("bad\nheader") && !RequestHeaders(L"api.github.com").empty(), "bad replacement preserves working token");

    HttpsClient api(L"api.github.com");
    Reset();
    Check(api.GetString(L"/repos/test/project/releases/latest", body, &err) && body == "{}", "authenticated release check");
    Check(reply.sentHeaders.find(L"Bearer github_pat_TEST_ONLY_12345") != std::wstring::npos, "token reaches actual Get request");
    Check(reply.redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "API redirects disabled");
    Set("ghp_REPLACEMENT");
    Reset();
    api.GetString(L"/repos/test/project/releases", body, &err);
    Check(reply.sentHeaders.find(L"ghp_REPLACEMENT") != std::wstring::npos, "live token change on existing client");
    HttpExtra extra;
    extra.headers = {L"Accept: application/vnd.github+json"};
    HttpResult result;
    Reset();
    Check(api.GetEx(L"/repos/test/project/releases", extra, result, &err) && result.status == 200, "GetEx still succeeds");
    Check(reply.sentHeaders.find(L"ghp_REPLACEMENT") != std::wstring::npos &&
          reply.sentHeaders.find(L"Accept:") != std::wstring::npos, "GetEx preserves extra headers with API token");
    Check(reply.redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "GetEx also blocks API redirects");
    Reset(); reply.rejectPolicy = true;
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && !reply.sent, "fail closed if redirect protection fails");
    Reset(); reply.rejectPolicy = true;
    Check(!api.GetEx(L"/repos/test/project/releases", extra, result, &err) && !reply.sent, "GetEx fails closed too");

    HttpsClient download(L"github.com");
    Reset();
    Check(download.GetString(L"/test/project/releases/download/v1/asset.zip", body, &err), "release download still works");
    Check(reply.sentHeaders.empty() && reply.redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER,
          "download stays anonymous with normal redirects");
    Set(""); Reset();
    Check(api.GetString(L"/repos/test/project/releases", body, &err) && reply.sentHeaders.empty(), "clear immediately restores anonymous requests");

    Reset(401, "server body containing " + fake);
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == ApiError(401, "", "", ""), "401 explains expired/invalid token");
    Check(err.find(fake) == std::string::npos, "server body does not leak into errors");
    Reset(403); reply.remaining = L"0";
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == ApiError(429, "", "", ""), "403 exhausted primary budget");
    Reset(403); reply.retryAfter = L"60";
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == ApiError(429, "", "", ""), "403 retry-after secondary limit");
    Reset(403, "{\"message\":\"You have exceeded a secondary rate limit\"}");
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == ApiError(429, "", "", ""), "403 body-only secondary limit");
    Reset(403); reply.remaining = L"100";
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err != ApiError(429, "", "", ""), "403 permission denial is not a rate limit");
    Reset(429);
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == ApiError(429, "", "", ""), "429 rate limit");
    Reset(404);
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == "HTTP 404", "unknown API errors retain status");
    Reset(302);
    Check(!api.GetString(L"/repos/test/project/releases", body, &err) && err == "HTTP 302", "API redirect is surfaced without following it");
    Reset(403, std::string(20000, 'x'));
    api.GetString(L"/repos/test/project/releases", body, &err);
    Check(reply.offset == 8192, "error-body classification is bounded");
    Reset(403, std::string(20000, 'x'));
    std::atomic<bool> cancel{true};
    api.GetString(L"/repos/test/project/releases", body, &err, &cancel);
    Check(reply.offset == 0, "shutdown cancels error-body reads");

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pobtools-github-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring file = root + L"PobTools\\github_token.bin";
    Check(Load(root, &err).empty() && err.empty(), "missing credential is normal");
    Check(Save(root, " \t" + fake + "\n", &err) && err.empty(), "save uses real Windows DPAPI");
    const std::string encrypted = Read(file);
    Check(!encrypted.empty() && encrypted.find(fake) == std::string::npos, "disk contains encrypted data only");
    Check(Load(root, &err) == fake && err.empty(), "saved credential round trip");
    Check(!Save(root, "bad\r\nheader", &err) && Load(root) == fake, "invalid save preserves previous credential");
    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock credential for failure test");
    Check(!Save(root, "ghp_NEW", &err) && !err.empty(), "failed replacement is reported");
    Check(!Save(root, "", &err) && !err.empty(), "failed deletion is reported");
    CloseHandle(locked);
    Check(Load(root) == fake, "failed replacement kept saved token intact");
    Check(Save(root, "", &err) && Load(root).empty(), "clear deletes encrypted file");
    Check(Save(root, "", &err), "clear is idempotent");
    HANDLE corrupt = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(corrupt, "invalid", 7, &written, nullptr);
    CloseHandle(corrupt);
    Check(Load(root, &err).empty() && !err.empty(), "corrupt credential produces recoverable error");
    Check(Save(root, fake, &err) && Load(root) == fake, "save repairs corrupt credential");
    Save(root, "");
    RemoveDirectoryW((root + L"PobTools").c_str());
    RemoveDirectoryW(root.c_str());

    // UI settings writes and worker reads must not observe a torn token.
    Set(fake);
    std::thread writer([&]() { for (int i = 0; i < 1000; ++i) Set(i % 2 ? fake : ""); });
    bool consistent = true;
    for (int i = 0; i < 1000; ++i) {
        const auto h = RequestHeaders(L"api.github.com");
        if (!h.empty() && h != L"Authorization: Bearer github_pat_TEST_ONLY_12345\r\n") consistent = false;
    }
    writer.join();
    Check(consistent, "concurrent updates produce complete credentials");
    Set("");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
