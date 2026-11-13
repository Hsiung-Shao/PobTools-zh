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

// Manual proxy for every host-side WinHTTP session ("host:port"; a pasted http:// prefix is tolerated). Empty =
// follow the system proxy (also the fallback when the manual value cannot be applied). Set at startup from
// pob-zh.ini and when the setting changes; sessions created afterwards pick it up.
void HttpSetManualProxy(const std::wstring& proxy);

// WinHttpOpen honouring the proxy policy above (HINTERNET as void*, null on failure). Every host-side WinHTTP
// session must come from here, or it silently bypasses the proxy setting.
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

// Selftest seam: while set, HttpsClient::Get/GetString answer from this instead of the network.
// Return true with the body in out, or false with *err set. cancel is the caller's flag (may be null).
// Set before any worker thread starts; pass nullptr to restore the network.
using HttpTestSource = std::function<bool(const std::wstring& host, const std::wstring& path,
                                          std::vector<unsigned char>& out, std::string* err,
                                          const std::atomic<bool>* cancel)>;
void HttpSetTestSource(HttpTestSource src);

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

	// GET with request headers and response-header collection. Unlike Get, a transport-level success returns true
	// for ANY status code -- the caller inspects out.status (401/403/429 bodies and headers carry the answer).
	// false = could not talk to the server at all.
	// Automatic WinHTTP cookies are disabled: only extra.headers' cookies are sent, and Set-Cookie cannot leak into later requests.
	bool GetEx(const std::wstring& path, const HttpExtra& extra, HttpResult& out,
	           std::string* err, const std::atomic<bool>* cancel = nullptr);

private:
	// One request. withToken=false never attaches the GitHub token, so WinHTTP may
	// follow redirects; *tokenGen is the generation of the token this attempt
	// actually carried (GitHubToken::RequestHeaders), 0 when it went anonymous.
	bool getOnce(const std::wstring& path, std::vector<unsigned char>& out,
	             std::string* err, const std::atomic<bool>* cancel,
	             const ProgressFn& onProgress, bool withToken, unsigned* tokenGen, int* status);
	bool getExOnce(const std::wstring& path, const HttpExtra& extra, HttpResult& out,
	               std::string* err, const std::atomic<bool>* cancel, bool withToken, unsigned* tokenGen);

	std::wstring host_;       // credentials are restricted to api.github.com
	void* hSession_ = nullptr; // HINTERNET
	void* hConnect_ = nullptr; // HINTERNET
};
