#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

HttpsClient::HttpsClient(const std::wstring& host)
{
	// GitHub's API rejects requests without a User-Agent; keep the product UA.
	HINTERNET s = WinHttpOpen(L"PobTools/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!s) return;
	// resolve / connect / send / receive timeouts: keep Shutdown-time joins short
	WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
	HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!c) {
		WinHttpCloseHandle(s);
		return;
	}
	hSession_ = s;
	hConnect_ = c;
}

HttpsClient::~HttpsClient()
{
	if (hConnect_) WinHttpCloseHandle((HINTERNET)hConnect_);
	if (hSession_) WinHttpCloseHandle((HINTERNET)hSession_);
}

bool HttpsClient::Get(const std::wstring& path, std::vector<unsigned char>& out,
                      std::string* err, const std::atomic<bool>* cancel,
                      const ProgressFn& onProgress)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	out.clear();
	if (!hConnect_) return fail(u8"HTTPS 連線初始化失敗");

	// paths arrive pre-encoded (%20 etc.); disable WinHTTP's re-escaping
	HINTERNET hReq = WinHttpOpenRequest((HINTERNET)hConnect_, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE);
	if (!hReq) return fail(u8"建立 HTTP 請求失敗");

	bool ok = false;
	std::string reason;
	if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hReq, nullptr)) {
		DWORD code = 0, len = sizeof(code);
		WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
		if (code == 200) {
			ok = true;
			unsigned long long total = 0;
			if (onProgress) {
				DWORD cl = 0, clLen = sizeof(cl);
				if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &cl, &clLen, WINHTTP_NO_HEADER_INDEX))
					total = cl;
			}
			DWORD avail = 0;
			do {
				if (cancel && cancel->load()) { ok = false; reason = u8"已取消"; break; }
				avail = 0;
				if (!WinHttpQueryDataAvailable(hReq, &avail)) { ok = false; reason = u8"讀取回應失敗"; break; }
				if (avail == 0) break;
				size_t off = out.size();
				out.resize(off + avail);
				DWORD rd = 0;
				if (!WinHttpReadData(hReq, out.data() + off, avail, &rd)) {
					out.resize(off);
					ok = false;
					reason = u8"讀取回應失敗";
					break;
				}
				out.resize(off + rd);
				if (onProgress) onProgress(out.size(), total);
			} while (avail > 0);
			if (ok && out.empty()) { ok = false; reason = u8"回應為空"; }
		} else {
			reason = "HTTP " + std::to_string(code) + ": " + narrow(path);
		}
	} else {
		reason = u8"連線失敗（網路無法使用？）: " + narrow(path);
	}
	WinHttpCloseHandle(hReq);
	if (!ok) {
		out.clear();
		return fail(reason);
	}
	return true;
}

bool HttpsClient::GetString(const std::wstring& path, std::string& out, std::string* err,
                            const std::atomic<bool>* cancel)
{
	std::vector<unsigned char> bytes;
	if (!Get(path, bytes, err, cancel)) return false;
	out.assign((const char*)bytes.data(), bytes.size());
	return true;
}
