// PobTools generic HTTPS GET client (WinHTTP).
//
// One instance per host, reused across requests. All calls are synchronous and
// intended for a worker thread (or a headless CLI); never call from the UI
// thread. Paths must already be percent-encoded (e.g. /Traditional%20Chinese/x).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ---- proxy -------------------------------------------------------------------
// Manual proxy for every host-side WinHTTP session ("host:port"; a pasted
// http:// prefix is tolerated). Empty = follow the system proxy automatically
// (the Clash / V2Ray "system proxy" toggle included), which is also the
// fallback when the manual value cannot be applied. Set at startup from
// pob-zh.ini and again when the setting changes; sessions created afterwards
// pick it up.
void HttpSetManualProxy(const std::wstring& proxy);

// WinHttpOpen honouring the proxy policy above (returns an HINTERNET as void*,
// null on failure). Every host-side WinHTTP session must come from here, or it
// silently bypasses the proxy setting.
void* HttpOpenSession(const wchar_t* userAgent);

// Extra request configuration for GetEx. Everything is optional; a
// default-constructed HttpExtra behaves like plain Get.
struct HttpExtra {
	// Sent verbatim, one "Name: value" per element. May carry credentials
	// (Cookie) -- so no caller and no error path may ever copy these into a
	// message or a log line.
	std::vector<std::wstring> headers;
	// Response headers to collect into HttpResult::headers, matched
	// case-insensitively. Empty = collect none.
	std::vector<std::wstring> wantHeaders;
};

struct HttpResult {
	int status = 0;                                       // 0 = transport failure
	// Keys are the wantHeaders names lower-cased; absent = header not present.
	std::unordered_map<std::string, std::string> headers;
	std::string body;                                     // filled for any status
};

class HttpsClient {
public:
	explicit HttpsClient(const std::wstring& host);
	~HttpsClient();
	HttpsClient(const HttpsClient&) = delete;
	HttpsClient& operator=(const HttpsClient&) = delete;

	bool valid() const { return hConnect_ != nullptr; }

	// Called after each received chunk with (bytes so far, Content-Length or 0
	// when the server did not send one). Runs on the calling (worker) thread.
	using ProgressFn = std::function<void(unsigned long long got, unsigned long long total)>;

	// GET https://<host><path>. Returns false on any non-200 status or transport
	// error and fills *err when provided. A non-null cancel flag aborts the body
	// read between chunks (used by worker shutdown).
	bool Get(const std::wstring& path, std::vector<unsigned char>& out,
	         std::string* err, const std::atomic<bool>* cancel = nullptr,
	         const ProgressFn& onProgress = nullptr);

	// Same, but into a string (for JSON / small text bodies).
	bool GetString(const std::wstring& path, std::string& out, std::string* err,
	               const std::atomic<bool>* cancel = nullptr);

	// GET with request headers and response-header collection. Unlike Get, a
	// transport-level success returns true for ANY status code -- the caller
	// inspects out.status (an API talks in 401/403/429 and their bodies and
	// headers carry the answer, so collapsing them into false would throw it
	// away). false = could not talk to the server at all.
	//
	// Automatic WinHTTP cookies are disabled for these requests: the only
	// cookies sent are what extra.headers spells out, and a Set-Cookie in the
	// response cannot leak into later requests.
	bool GetEx(const std::wstring& path, const HttpExtra& extra, HttpResult& out,
	           std::string* err, const std::atomic<bool>* cancel = nullptr);

private:
	void* hSession_ = nullptr; // HINTERNET
	void* hConnect_ = nullptr; // HINTERNET
};
