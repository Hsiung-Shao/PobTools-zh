// PobTools generic HTTPS GET client (WinHTTP).
//
// One instance per host, reused across requests. All calls are synchronous and
// intended for a worker thread (or a headless CLI); never call from the UI
// thread. Paths must already be percent-encoded (e.g. /Traditional%20Chinese/x).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

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

private:
	void* hSession_ = nullptr; // HINTERNET
	void* hConnect_ = nullptr; // HINTERNET
};
