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
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* name)
{
	++checks;
	if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}

// Every WinHttpOpenRequest starts a new request; the later WinHttpSetOption /
// WinHttpSendRequest / reads act on the most recently opened one.
struct Req {
	DWORD status = 200;
	std::string body = "{}";
	std::wstring sentHeaders;
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	size_t offset = 0;
	bool sent = false;
};
struct Config {
	DWORD status = 200;                  // used once the script runs out
	std::string body = "{}";
	std::wstring remaining, retryAfter;
	bool rejectPolicy = false;
	std::deque<std::pair<DWORD, std::string>> script; // per-request status/body, in order
} cfg;
std::vector<Req> reqs;

void Reset(DWORD status = 200, const std::string& body = "{}")
{
	cfg = Config{};
	cfg.status = status;
	cfg.body = body;
	reqs.clear();
}

const Req& Last()
{
	static const Req none;
	return reqs.empty() ? none : reqs.back();
}
bool AnySent()
{
	return std::any_of(reqs.begin(), reqs.end(), [](const Req& r) { return r.sent; });
}
bool HasText(const std::wstring& haystack, const wchar_t* needle)
{
	return haystack.find(needle) != std::wstring::npos;
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
	Req r;
	if (!cfg.script.empty()) {
		r.status = cfg.script.front().first;
		r.body = cfg.script.front().second;
		cfg.script.pop_front();
	} else {
		r.status = cfg.status;
		r.body = cfg.body;
	}
	reqs.push_back(r);
	return (HINTERNET)3;
}
BOOL WINAPI WinHttpCloseHandle(HINTERNET) { return TRUE; }
BOOL WINAPI WinHttpSetTimeouts(HINTERNET, int, int, int, int) { return TRUE; }
BOOL WINAPI WinHttpSetOption(HINTERNET, DWORD option, LPVOID value, DWORD)
{
	if (option == WINHTTP_OPTION_REDIRECT_POLICY && !reqs.empty()) {
		if (cfg.rejectPolicy) return FALSE;
		reqs.back().redirectPolicy = *(DWORD*)value;
	}
	return TRUE;
}
BOOL WINAPI WinHttpSendRequest(HINTERNET, LPCWSTR headers, DWORD size, LPVOID, DWORD, DWORD, DWORD_PTR)
{
	if (reqs.empty()) return FALSE;
	reqs.back().sent = true;
	reqs.back().sentHeaders = headers ? std::wstring(headers, size) : L"";
	return TRUE;
}
BOOL WINAPI WinHttpReceiveResponse(HINTERNET, LPVOID) { return TRUE; }
BOOL WINAPI WinHttpQueryHeaders(HINTERNET, DWORD query, LPCWSTR name, LPVOID buffer, LPDWORD size, LPDWORD)
{
	if (query == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
		*(DWORD*)buffer = Last().status;
		return TRUE;
	}
	if (query != WINHTTP_QUERY_CUSTOM) return FALSE;
	const std::wstring value = _wcsicmp(name, L"Retry-After") == 0 ? cfg.retryAfter : cfg.remaining;
	if (value.empty() || *size < (value.size() + 1) * sizeof(wchar_t)) return FALSE;
	std::memcpy(buffer, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
	*size = (DWORD)(value.size() * sizeof(wchar_t));
	return TRUE;
}
BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET, LPDWORD size)
{
	const Req& r = Last();
	*size = (DWORD)(r.body.size() - r.offset);
	return TRUE;
}
BOOL WINAPI WinHttpReadData(HINTERNET, LPVOID buffer, DWORD size, LPDWORD read)
{
	Req& r = reqs.back();
	*read = (DWORD)(std::min)((size_t)size, r.body.size() - r.offset);
	std::memcpy(buffer, r.body.data() + r.offset, *read);
	r.offset += *read;
	return TRUE;
}
} // extern "C"

int main()
{
	using namespace GitHubToken;
	const std::string fake = "github_pat_TEST_ONLY_12345";
	const std::wstring releases = L"/repos/test/project/releases";
	std::string token, err, body;

	// ---- Normalize ----
	Check(Normalize(" \r\n" + fake + "\t", token) && token == fake, "貼上的前後空白會被去除");
	Check(Normalize(token, token) && token == fake, "輸入輸出同一變數時仍可正規化");
	Check(Normalize(" \r\n", token) && token.empty(), "純空白視為匿名");
	Check(Normalize("", token) && token.empty(), "空字串視為匿名");
	for (const std::string& bad : std::vector<std::string>{"ghp_x\r\nInjected: value", "Bearer ghp_x", "ghp_x y", "\xC3\xA9", "ghp_" + std::string(4097, 'x')})
		Check(!Normalize(bad, token) && token.empty(), "標頭注入、錯誤前綴與非法輸入一律拒絕");
	// T4
	for (const std::string& bad : std::vector<std::string>{"abc123", "gho_abc", "ghs_abc", "token_abc"}) {
		token = "stale";
		Check(!Normalize(bad, token) && token.empty(), "非 PAT 前綴被拒絕且輸出為空");
	}
	// T5
	for (const std::string& good : std::vector<std::string>{"ghp_abc123", "github_pat_11ABC_def"}) {
		token.clear();
		Check(Normalize(good, token) && token == good, "ghp_ 與 github_pat_ 前綴可通過");
	}

	// ---- 請求標頭 ----
	Check(Set(fake), "設定合法 token 成功");
	Check(RequestHeaders(L"API.GITHUB.COM") == L"Authorization: Bearer github_pat_TEST_ONLY_12345\r\n", "API 主機帶 Bearer 標頭");
	for (const wchar_t* host : {L"github.com", L"raw.githubusercontent.com", L"objects.githubusercontent.com",
	                            L"api.github.com.evil.example", L"evil-api.github.com", L"poe.ninja"})
		Check(RequestHeaders(host).empty(), "下載站、仿冒網域與其他服務不帶憑證");
	Check(!Set("bad\nheader") && !RequestHeaders(L"api.github.com").empty(), "替換值不合法時保留原 token");

	// ---- T2 帶 token 時禁止轉址 ----
	HttpsClient api(L"api.github.com");
	Reset();
	Check(api.GetString(releases + L"/latest", body, &err) && body == "{}", "帶 token 的版本檢查成功");
	Check(HasText(Last().sentHeaders, L"Bearer github_pat_TEST_ONLY_12345"), "token 實際送進 Get 請求");
	Check(Last().redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "帶 token 時 Get 禁止轉址");
	Set("ghp_REPLACEMENT");
	Reset();
	api.GetString(releases, body, &err);
	Check(HasText(Last().sentHeaders, L"ghp_REPLACEMENT"), "既有 client 即時套用新 token");
	HttpExtra extra;
	extra.headers = {L"Accept: application/vnd.github+json"};
	HttpResult result;
	Reset();
	Check(api.GetEx(releases, extra, result, &err) && result.status == 200, "帶 token 的 GetEx 成功");
	Check(HasText(Last().sentHeaders, L"Bearer ghp_REPLACEMENT") &&
	      HasText(Last().sentHeaders, L"Accept:"), "GetEx 帶 token 時保留額外標頭");
	Check(Last().redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "帶 token 時 GetEx 禁止轉址");
	Reset(); cfg.rejectPolicy = true;
	Check(!api.GetString(releases, body, &err) && !AnySent(), "停用轉址失敗時 Get 不送出請求");
	Reset(); cfg.rejectPolicy = true;
	Check(!api.GetEx(releases, extra, result, &err) && !AnySent(), "停用轉址失敗時 GetEx 不送出請求");

	// ---- T3 token 收到 3xx 時匿名重試 ----
	for (DWORD redirect : {301u, 302u, 307u, 308u}) {
		Set("ghp_REPLACEMENT");
		Reset();
		cfg.script = {{redirect, ""}, {200, "{}"}};
		body.clear();
		Check(api.GetString(releases, body, &err) && body == "{}", "Get 收到 3xx 後匿名重試成功");
		Check(reqs.size() == 2, "Get 收到 3xx 共送出 2 次請求");
		Check(reqs.size() == 2 && HasText(reqs[0].sentHeaders, L"Bearer ghp_REPLACEMENT") &&
		      reqs[0].redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "Get 第 1 次請求帶 token 且禁止轉址");
		Check(reqs.size() == 2 && !HasText(reqs[1].sentHeaders, L"Authorization") &&
		      reqs[1].redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "Get 重試不帶 Authorization 且允許轉址");

		Reset();
		cfg.script = {{redirect, ""}, {200, "{}"}};
		HttpResult retried;
		Check(api.GetEx(releases, extra, retried, &err) && retried.status == 200 && retried.body == "{}", "GetEx 收到 3xx 後匿名重試成功");
		Check(reqs.size() == 2, "GetEx 收到 3xx 共送出 2 次請求");
		Check(reqs.size() == 2 && HasText(reqs[0].sentHeaders, L"Bearer ghp_REPLACEMENT") &&
		      reqs[0].redirectPolicy == WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "GetEx 第 1 次請求帶 token 且禁止轉址");
		Check(reqs.size() == 2 && !HasText(reqs[1].sentHeaders, L"Authorization") &&
		      reqs[1].redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "GetEx 重試不帶 Authorization 且允許轉址");
	}
	// 匿名重試也一直回 302:保留狀態碼與路徑
	Set("ghp_REPLACEMENT");
	Reset(302);
	Check(!api.GetString(releases, body, &err) && err == "HTTP 302: /repos/test/project/releases", "重試仍回 302 時錯誤保留狀態與路徑");

	// ---- 下載站維持匿名且正常轉址 ----
	HttpsClient download(L"github.com");
	Reset();
	Check(download.GetString(L"/test/project/releases/download/v1/asset.zip", body, &err), "下載站請求成功");
	Check(Last().sentHeaders.empty() && Last().redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER,
	      "下載站維持匿名且正常轉址");

	// ---- T1 未設 token 時 API 照常轉址 ----
	Set(""); Reset();
	Check(api.GetString(releases, body, &err) && Last().sentHeaders.empty(), "清除 token 後立即回到匿名請求");
	Reset();
	Check(api.GetString(releases, body, &err), "未設 token 時 Get 成功");
	Check(Last().redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER &&
	      !HasText(Last().sentHeaders, L"Authorization"), "未設 token 時 Get 照常轉址且不帶 Authorization");
	Reset();
	Check(api.GetEx(releases, extra, result, &err) && result.status == 200, "未設 token 時 GetEx 成功");
	Check(Last().redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER &&
	      !HasText(Last().sentHeaders, L"Authorization"), "未設 token 時 GetEx 照常轉址且不帶 Authorization");

	// ---- API 錯誤分類(帶 token 的情境;每案先重設 token,避免被拒旗標影響) ----
	const auto Authed = []() { Set("ghp_REPLACEMENT"); };
	Authed(); Reset(401, "server body containing " + fake);
	Check(!api.GetString(releases, body, &err) && err == ApiError(401, "", "", "", true), "401 提示 token 失效");
	Check(err.find(fake) == std::string::npos, "伺服器回應內容不會洩漏進錯誤訊息");
	Authed(); Reset(403); cfg.remaining = L"0";
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", true), "403 主要額度用盡視為限流");
	Authed(); Reset(403); cfg.retryAfter = L"60";
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", true), "403 帶 Retry-After 視為次級限流");
	Authed(); Reset(403, "{\"message\":\"You have exceeded a secondary rate limit\"}");
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", true), "403 僅內文提示次級限流仍視為限流");
	Authed(); Reset(403); cfg.remaining = L"100";
	Check(!api.GetString(releases, body, &err) && err != ApiError(429, "", "", "", true), "403 權限不足不視為限流");
	Authed(); Reset(429);
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", true), "429 視為限流");

	// T9 帶 token 收到 401 時匿名重試
	Authed(); Reset();
	cfg.script = {{401, ""}, {200, "{}"}};
	body.clear();
	Check(api.GetString(releases, body, &err) && body == "{}", "Get 收到 401 後匿名重試成功");
	Check(reqs.size() == 2, "Get 收到 401 共送出 2 次請求");
	Check(reqs.size() == 2 && HasText(reqs[0].sentHeaders, L"Bearer ghp_REPLACEMENT"), "Get 第 1 次請求帶 token");
	Check(reqs.size() == 2 && !HasText(reqs[1].sentHeaders, L"Authorization") &&
	      reqs[1].redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "Get 401 重試不帶 Authorization 且允許轉址");
	Check(Rejected(), "Get 收到 401 後標記 token 被拒");
	Authed(); Reset();
	cfg.script = {{401, ""}, {200, "{}"}};
	HttpResult after401;
	Check(api.GetEx(releases, extra, after401, &err) && after401.status == 200 && after401.body == "{}", "GetEx 收到 401 後匿名重試成功");
	Check(reqs.size() == 2, "GetEx 收到 401 共送出 2 次請求");
	Check(reqs.size() == 2 && HasText(reqs[0].sentHeaders, L"Bearer ghp_REPLACEMENT"), "GetEx 第 1 次請求帶 token");
	Check(reqs.size() == 2 && !HasText(reqs[1].sentHeaders, L"Authorization") &&
	      reqs[1].redirectPolicy != WINHTTP_OPTION_REDIRECT_POLICY_NEVER, "GetEx 401 重試不帶 Authorization 且允許轉址");
	Check(Rejected(), "GetEx 收到 401 後標記 token 被拒");

	// T10 換 token 或清空後清除被拒旗標
	Check(Rejected(), "換 token 前仍為被拒");
	Set("ghp_ANOTHER_TOKEN");
	Check(!Rejected(), "換 token 後清除被拒旗標");
	Authed(); Reset();
	cfg.script = {{401, ""}, {200, "{}"}};
	api.GetString(releases, body, &err);
	Check(Rejected(), "再次收到 401 又標記被拒");
	Set("");
	Check(!Rejected(), "清空 token 後清除被拒旗標");

	// T11 401 且匿名重試也失敗時回報 token 無效
	Authed(); Reset(403); cfg.remaining = L"0";
	cfg.script = {{401, ""}};
	Check(!api.GetString(releases, body, &err) && err == ApiError(401, "", "", "", true), "401 後匿名重試也失敗時回報 token 無效");
	Check(reqs.size() == 2, "401 後匿名重試也失敗共送出 2 次請求");

	// T12 訊息依有無帶 token 分流
	Check(!ApiError(401, "", "", "", true).empty(), "帶 token 的 401 有訊息");
	Check(ApiError(401, "", "", "", false).empty(), "匿名的 401 沒有專屬訊息");
	const std::string limitedWith = ApiError(429, "", "", "", true), limitedWithout = ApiError(429, "", "", "", false);
	Check(!limitedWith.empty() && !limitedWithout.empty() && limitedWith != limitedWithout, "限流訊息依有無 token 不同");
	Check(limitedWith.find(u8"儲存") == std::string::npos, "帶 token 的限流訊息不建議儲存 token");
	Check(!ApiError(403, "100", "", "", false).empty() && ApiError(403, "100", "", "", false).find("Token") == std::string::npos,
	      "匿名的 403 訊息不提 Token");
	Check(!ApiError(403, "100", "", "", true).empty(), "帶 token 的 403 有訊息");
	Set(""); Reset(429);
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", false), "匿名收到 429 回匿名版訊息");
	Authed(); Reset(429);
	Check(!api.GetString(releases, body, &err) && err == ApiError(429, "", "", "", true), "帶 token 收到 429 回帶 token 版訊息");

	// T6
	Set(""); Reset(404);
	Check(!api.GetString(releases, body, &err) && err == "HTTP 404: /repos/test/project/releases", "未知狀態碼錯誤保留狀態與路徑");
	Reset(403, std::string(20000, 'x'));
	api.GetString(releases, body, &err);
	Check(Last().offset == 8192, "錯誤內文分類最多讀 8192 位元組");
	Reset(403, std::string(20000, 'x'));
	std::atomic<bool> cancel{true};
	api.GetString(releases, body, &err, &cancel);
	Check(Last().offset == 0, "關閉中不再讀取錯誤內文");

	// ---- 加密儲存 ----
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	const std::wstring root = std::wstring(temp) + L"pobtools-github-test-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
	CreateDirectoryW(root.c_str(), nullptr);
	const std::wstring file = root + L"PobTools\\github_token.bin";
	Check(Load(root, &err).empty() && err.empty(), "無已存憑證屬正常情況");
	Check(Save(root, " \t" + fake + "\n", &err) && err.empty(), "以 Windows DPAPI 儲存成功");
	const std::string encrypted = Read(file);
	Check(!encrypted.empty() && encrypted.find(fake) == std::string::npos, "磁碟上只有密文");
	Check(Load(root, &err) == fake && err.empty(), "儲存後可原樣讀回");
	Check(!Save(root, "bad\r\nheader", &err) && Load(root) == fake, "儲存不合法值時保留原憑證");
	HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	Check(locked != INVALID_HANDLE_VALUE, "鎖住憑證檔以模擬失敗");
	Check(!Save(root, "ghp_NEW", &err) && !err.empty(), "替換失敗會回報錯誤");
	Check(!Save(root, "", &err) && !err.empty(), "刪除失敗會回報錯誤");
	CloseHandle(locked);
	Check(Load(root) == fake, "替換失敗後原憑證完好");
	Check(Save(root, "", &err) && Load(root).empty(), "清除會刪除加密檔");
	Check(Save(root, "", &err), "重複清除不報錯");
	HANDLE corrupt = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	DWORD written = 0;
	WriteFile(corrupt, "invalid", 7, &written, nullptr);
	CloseHandle(corrupt);
	Check(Load(root, &err).empty() && !err.empty(), "憑證損毀時回報可重新儲存的錯誤");
	Check(Save(root, fake, &err) && Load(root) == fake, "重新儲存可修復損毀憑證");
	// T7
	Save(root, "");
	HANDLE emptyFile = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	Check(emptyFile != INVALID_HANDLE_VALUE, "建立 0 位元組憑證檔");
	CloseHandle(emptyFile);
	err.clear();
	Check(Load(root, &err).empty() && err == u8"已儲存的 GitHub Token 格式無效，請重新儲存", "空檔視為格式無效");
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
	Check(consistent, "並行更新不會讀到殘缺憑證");
	// T13 另一程序改檔後下一次請求就用新 token(Init 狀態殘留,故放最後)
	const std::string tokenA = "ghp_TOKEN_A_111", tokenB = "ghp_TOKEN_B_222";
	CreateDirectoryW(root.c_str(), nullptr);
	Check(Save(root, tokenA, &err), "T13 先存 token A");
	Init(root);
	Check(HasText(RequestHeaders(L"api.github.com"), L"ghp_TOKEN_A_111"), "Init 後請求帶 token A");
	Sleep(50);
	Check(Save(root, tokenB, &err), "T13 模擬另一程序存 token B");
	const std::wstring afterB = RequestHeaders(L"api.github.com");
	Check(HasText(afterB, L"ghp_TOKEN_B_222") && !HasText(afterB, L"ghp_TOKEN_A_111"), "檔案被改後下一次請求用 token B");
	Sleep(50);
	Check(Save(root, "", &err), "T13 模擬另一程序清除 token");
	Check(RequestHeaders(L"api.github.com").empty(), "檔案被清除後下一次請求匿名");
	RemoveDirectoryW((root + L"PobTools").c_str());
	RemoveDirectoryW(root.c_str());

	Set("");
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
