#include "http_client.h"
#include "error_log.h"
#include "github_token.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <mutex>

#pragma comment(lib, "winhttp.lib")

// Absent from older SDK headers; the value is stable (Win 8.1+ feature).
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// Manual proxy shared by every session this process opens. Guarded because the
// settings page writes it on the UI thread while workers open sessions.
static std::mutex g_proxyMx;
static std::wstring g_manualProxy; // normalized "host:port"; empty = automatic

void HttpSetManualProxy(const std::wstring& proxy)
{
	std::wstring p = proxy;
	while (!p.empty() && iswspace(p.front())) p.erase(p.begin());
	while (!p.empty() && iswspace(p.back())) p.pop_back();
	// People paste what their proxy tool shows, which is a URL.
	auto stripPrefix = [&p](const wchar_t* pre) {
		size_t n = wcslen(pre);
		if (p.size() >= n && _wcsnicmp(p.c_str(), pre, n) == 0) p.erase(0, n);
	};
	stripPrefix(L"http://");
	stripPrefix(L"https://");
	while (!p.empty() && p.back() == L'/') p.pop_back();
	std::lock_guard<std::mutex> lk(g_proxyMx);
	g_manualProxy = p;
}

void* HttpOpenSession(const wchar_t* userAgent)
{
	std::wstring proxy;
	{
		std::lock_guard<std::mutex> lk(g_proxyMx);
		proxy = g_manualProxy;
	}
	if (!proxy.empty()) {
		HINTERNET s = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
			proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
		if (s) return s;
		PobLog::Error("net", "manual proxy '" + narrow(proxy) +
		                         "' rejected by WinHttpOpen, falling back to the system proxy");
	}
	// Follow the system proxy (IE/WinINET settings + WPAD): this is what makes
	// a Clash/V2Ray "system proxy" work without any setting. Win 8.1+; older
	// Windows rejects the flag and drops to the historical direct behaviour.
	HINTERNET s = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!s)
		s = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	return s;
}

HttpsClient::HttpsClient(const std::wstring& host) : host_(host)
{
	// GitHub's API rejects requests without a User-Agent; keep the product UA.
	HINTERNET s = (HINTERNET)HttpOpenSession(L"PobTools/1.0");
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

// A request carrying the token must never forward it to a redirect destination,
// so only those requests stop following redirects. Anonymous ones (no token set,
// or the retry below) keep WinHTTP's normal behaviour: a renamed repository's 301
// still resolves. Release assets use separate github.com/CDN clients and never
// see the token at all.
static bool github_request(HINTERNET request, const std::wstring& host, bool withToken,
                           std::wstring& headers, unsigned* tokenGen)
{
	*tokenGen = 0;
	if (!withToken) return true;
	unsigned gen = 0;
	std::wstring auth = GitHubToken::RequestHeaders(host, &gen);
	if (auth.empty()) return true;
	DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return false;
	headers = std::move(auth);
	*tokenGen = gen;
	return true;
}

static bool is_redirect(int code)
{
	return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

// A 401 to an authenticated API request: remember it and return the message to
// show if the anonymous retry fails as well.
static std::string token_rejected(const std::wstring& path, unsigned tokenGen)
{
	GitHubToken::NoteRejected(tokenGen);
	PobLog::Error("net", "HTTP 401 with GitHub token, retrying anonymously: " + narrow(path));
	return GitHubToken::ApiError(401, "", "", "", true);
}

static std::string response_header(HINTERNET request, const wchar_t* name)
{
	wchar_t value[128]{};
	DWORD size = sizeof(value);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value, &size, WINHTTP_NO_HEADER_INDEX))
		return {};
	return narrow(std::wstring(value, size / sizeof(wchar_t)));
}

bool HttpsClient::Get(const std::wstring& path, std::vector<unsigned char>& out,
                      std::string* err, const std::atomic<bool>* cancel,
                      const ProgressFn& onProgress)
{
	// true always means "no error": a retry that succeeds must not leave the
	// first attempt's message behind.
	if (err) err->clear();
	unsigned tokenGen = 0;
	int status = 0;
	if (getOnce(path, out, err, cancel, onProgress, true, &tokenGen, &status)) return true;
	if (!tokenGen) return false;
	// Redirected (repository renamed or moved): ask again without the token and
	// let WinHTTP follow it.
	if (is_redirect(status)) {
		if (err) err->clear();
		return getOnce(path, out, err, cancel, onProgress, false, nullptr, nullptr);
	}
	// Token expired or revoked: the request itself is public, so a dead token
	// must not stop update checks. Flag it for the settings page and go on
	// anonymously; if that fails too, the token is the thing to fix.
	if (status == 401) {
		const std::string invalid = token_rejected(path, tokenGen);
		std::string retryErr;
		if (getOnce(path, out, &retryErr, cancel, onProgress, false, nullptr, nullptr)) {
			if (err) err->clear();
			return true;
		}
		PobLog::Error("net", "anonymous retry failed too: " + retryErr);
		if (err) *err = invalid;
	}
	return false;
}

bool HttpsClient::getOnce(const std::wstring& path, std::vector<unsigned char>& out,
                          std::string* err, const std::atomic<bool>* cancel,
                          const ProgressFn& onProgress, bool withToken, unsigned* tokenGen, int* status)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	out.clear();
	if (status) *status = 0;
	if (!hConnect_) return fail(u8"HTTPS 連線初始化失敗");

	// paths arrive pre-encoded (%20 etc.); disable WinHTTP's re-escaping
	HINTERNET hReq = WinHttpOpenRequest((HINTERNET)hConnect_, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE);
	if (!hReq) return fail(u8"建立 HTTP 請求失敗");
	std::wstring headers;
	unsigned gen = 0;
	if (!github_request(hReq, host_, withToken, headers, &gen)) {
		WinHttpCloseHandle(hReq);
		return fail(u8"建立 HTTP 請求失敗");
	}
	const bool tokenSent = gen != 0;
	if (tokenGen) *tokenGen = gen;

	bool ok = false;
	std::string reason;
	if (WinHttpSendRequest(hReq, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
		headers.empty() ? 0 : (DWORD)headers.size(), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hReq, nullptr)) {
		DWORD code = 0, len = sizeof(code);
		WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
		if (status) *status = (int)code;
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
			if (GitHubToken::IsApiHost(host_)) {
				// Secondary limits may only be named in the body. Read at most 8 KiB
				// for classification; never include a server body in a log or message.
				std::string body;
				if (code == 403) {
					char chunk[1024];
					DWORD read = 0;
					while (body.size() < 8192 && !(cancel && cancel->load()) &&
					       WinHttpReadData(hReq, chunk, sizeof(chunk), &read) && read)
						body.append(chunk, read);
				}
				std::string friendly = GitHubToken::ApiError((int)code,
					response_header(hReq, L"X-RateLimit-Remaining"),
					response_header(hReq, L"Retry-After"), body, tokenSent);
				if (!friendly.empty()) {
					// (A 401 that carried the token is logged once, by token_rejected.)
					// The friendly wording drops the status and path; keep both in
					// the error log so a report still says which request failed.
					if (!(code == 401 && tokenSent)) PobLog::Error("net", reason);
					reason = std::move(friendly);
				}
			}
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

bool HttpsClient::GetEx(const std::wstring& path, const HttpExtra& extra, HttpResult& out,
                        std::string* err, const std::atomic<bool>* cancel)
{
	if (err) err->clear();
	unsigned tokenGen = 0;
	if (!getExOnce(path, extra, out, err, cancel, true, &tokenGen)) return false;
	// Same as Get: a redirected or rejected (401) authenticated request is
	// retried anonymously; the caller reads the retry's status as usual.
	if (tokenGen && (is_redirect(out.status) || out.status == 401)) {
		if (out.status == 401) token_rejected(path, tokenGen);
		return getExOnce(path, extra, out, err, cancel, false, nullptr);
	}
	return true;
}

bool HttpsClient::getExOnce(const std::wstring& path, const HttpExtra& extra, HttpResult& out,
                            std::string* err, const std::atomic<bool>* cancel, bool withToken,
                            unsigned* tokenGen)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	out = HttpResult{};
	if (!hConnect_) return fail(u8"HTTPS 連線初始化失敗");

	HINTERNET hReq = WinHttpOpenRequest((HINTERNET)hConnect_, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE);
	if (!hReq) return fail(u8"建立 HTTP 請求失敗");

	// The only cookies on the wire are the ones extra.headers spells out. Without
	// this, WinHTTP keeps a per-session jar: a Set-Cookie from one response would
	// ride along on every later request through this client.
	DWORD disable = WINHTTP_DISABLE_COOKIES;
	WinHttpSetOption(hReq, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

	std::wstring joined;
	unsigned gen = 0;
	if (!github_request(hReq, host_, withToken, joined, &gen)) {
		WinHttpCloseHandle(hReq);
		return fail(u8"建立 HTTP 請求失敗");
	}
	if (tokenGen) *tokenGen = gen;
	for (const std::wstring& h : extra.headers) {
		joined += h;
		joined += L"\r\n";
	}

	bool ok = false;
	std::string reason;
	if (WinHttpSendRequest(hReq,
			joined.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : joined.c_str(),
			joined.empty() ? 0 : (DWORD)joined.size(),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hReq, nullptr)) {
		DWORD code = 0, len = sizeof(code);
		WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
		out.status = (int)code;

		for (const std::wstring& name : extra.wantHeaders) {
			wchar_t buf[1024];
			DWORD blen = sizeof(buf);
			// WinHttpQueryHeaders wants a mutable name buffer for CUSTOM queries.
			std::wstring nameCopy = name;
			if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM, nameCopy.c_str(),
					buf, &blen, WINHTTP_NO_HEADER_INDEX)) {
				std::string key = narrow(name);
				for (char& c : key) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
				out.headers[key] = narrow(std::wstring(buf, blen / sizeof(wchar_t)));
			}
		}

		// The body is read for every status: 429's Retry-After story and 403's
		// Cloudflare page are exactly the bodies a caller needs to see.
		ok = true;
		DWORD avail = 0;
		do {
			if (cancel && cancel->load()) { ok = false; reason = u8"已取消"; break; }
			avail = 0;
			if (!WinHttpQueryDataAvailable(hReq, &avail)) { ok = false; reason = u8"讀取回應失敗"; break; }
			if (avail == 0) break;
			size_t off = out.body.size();
			out.body.resize(off + avail);
			DWORD rd = 0;
			if (!WinHttpReadData(hReq, &out.body[off], avail, &rd)) {
				out.body.resize(off);
				ok = false;
				reason = u8"讀取回應失敗";
				break;
			}
			out.body.resize(off + rd);
		} while (avail > 0);
	} else {
		reason = u8"連線失敗（網路無法使用？）: " + narrow(path);
	}
	WinHttpCloseHandle(hReq);
	if (!ok) {
		out = HttpResult{};
		return fail(reason);
	}
	return true;
}
