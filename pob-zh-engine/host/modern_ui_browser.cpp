#include "modern_ui_browser.h"

#include "app_version.h"
#include "bridge_gate.h"
#include "error_log.h"
#include "headless_proc.h"
#include "launcher_config.h"
#include "pob_launch.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

namespace {

// The page is gone when no event stream has been open for this long (a reload
// reconnects within a second or two), or when none ever opened.
constexpr int kGoneSeconds = 20;
constexpr int kFirstConnectSeconds = 180;

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring widen(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	out.clear();
	char buf[65536];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return true;
}

std::string random_token()
{
	static const char hex[] = "0123456789abcdef";
	// std::random_device is the OS generator on MSVC (and on Wine's CRT)
	std::random_device rd;
	std::string t;
	for (int i = 0; i < 32; i++) t += hex[rd() & 15];
	return t;
}

const char* mime_for(const std::wstring& path)
{
	auto dot = path.find_last_of(L'.');
	std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot + 1);
	for (auto& c : ext) c = (wchar_t)towlower(c);
	if (ext == L"html") return "text/html; charset=utf-8";
	if (ext == L"js" || ext == L"mjs") return "text/javascript; charset=utf-8";
	if (ext == L"css") return "text/css; charset=utf-8";
	if (ext == L"json") return "application/json; charset=utf-8";
	if (ext == L"png") return "image/png";
	if (ext == L"jpg" || ext == L"jpeg") return "image/jpeg";
	if (ext == L"webp") return "image/webp";
	if (ext == L"svg") return "image/svg+xml";
	if (ext == L"ttf") return "font/ttf";
	if (ext == L"otf") return "font/otf";
	if (ext == L"woff2") return "font/woff2";
	if (ext == L"ico") return "image/x-icon";
	return "application/octet-stream";
}

std::string url_decode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
			out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

bool send_all(SOCKET s, const std::string& data)
{
	size_t off = 0;
	while (off < data.size()) {
		int n = send(s, data.data() + off, (int)(data.size() - off), 0);
		if (n <= 0) return false;
		off += (size_t)n;
	}
	return true;
}

void respond(SOCKET s, int code, const char* status, const char* type, const std::string& body)
{
	char head[512];
	snprintf(head, sizeof(head),
	         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
	         "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
	         code, status, type, body.size());
	send_all(s, head);
	send_all(s, body);
}

struct Server {
	std::wstring exeDir, game, locale, openBuild, pobDir, launchLua;
	LauncherConfig cfg;
	std::string token;
	int port = 0;
	SOCKET listener = INVALID_SOCKET;
	std::unique_ptr<HeadlessProc::Child> child;

	// Everything the page must receive, in order. Each stream keeps its own
	// read position; `base` is the sequence number of queue.front().
	std::mutex mu;
	std::condition_variable cv;
	std::deque<std::string> queue;
	size_t base = 0;
	int streams = 0;
	bool everConnected = false;
	std::chrono::steady_clock::time_point lastStreamSeen = std::chrono::steady_clock::now();
	std::atomic<bool> quit{ false };
	int exitCode = 0;
	bool gateFellBack = false;

	std::string prefix() const { return "/t/" + token + "/"; }
	std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }

	void push(const std::string& line)
	{
		{
			std::lock_guard<std::mutex> lk(mu);
			queue.push_back(line);
			// Keep the history bounded; a page that reconnects after this many
			// lines reloads from scratch anyway (the bridge answers fresh).
			while (queue.size() > 20000) { queue.pop_front(); base++; }
		}
		cv.notify_all();
	}

	void event(const char* name, const json& data) { push(json{ {"event", name}, {"data", data} }.dump()); }

	json prefs() const
	{
		return json{ {"zoom", ClampModernZoom(cfg.modernZoom)}, {"fontSize", ClampModernFontSize(cfg.modernFontSize)} };
	}

	json info() const
	{
		wchar_t view[64] = {};
		GetEnvironmentVariableW(L"POB_ZH_UI_VIEW", view, 64);
		const std::string b = origin() + prefix();
		return json{
			{"game", narrow(game)}, {"locale", narrow(locale)}, {"exeDir", narrow(exeDir)}, {"pobDir", narrow(pobDir)},
			{"version", POBTOOLS_VERSION_STRING}, {"open", narrow(openBuild)}, {"view", narrow(view)},
			{"prefs", prefs()}, {"browser", true},
			{"hosts", json{ {"app", b.substr(0, b.size() - 1)}, {"pob", b + "~pob"}, {"data", b + "~data"},
			                {"fonts", b + "~fonts"}, {"cache", b + "~cache"} }},
		};
	}

	// The same compatibility gate the WebView2 window applies (OnGateLine there).
	bool on_gate_line(const std::string& line)
	{
		if (line.find("\"gate_result\"") == std::string::npos) return false;
		json msg;
		try { msg = json::parse(line); } catch (...) { return false; }
		if (!msg.is_object() || msg.value("event", "") != "gate_result") return false;
		const json& d = msg.contains("data") ? msg["data"] : json::object();
		BridgeGate::Verdict v;
		v.ok = d.value("ok", false);
		if (d.contains("failed") && d["failed"].is_array())
			for (auto& f : d["failed"]) if (f.is_string()) v.failed.push_back(f.get<std::string>());
		v.pobVersion = d.value("pobVersion", "");
		v.pobBranch = d.value("pobBranch", "");
		v.pobDir = pobDir;
		v.bridgeHash = BridgeGate::BridgeFingerprint(exeDir);
		BridgeGate::Write(exeDir, v);
		if (v.ok || gateFellBack) return false;
		gateFellBack = true;
		PobLog::Error("modernui", "browser mode: bridge gate failed for POB " + v.pobVersion + " -- opening the classic window instead");
		event("host.gate_fallback", json{ {"failed", v.failed}, {"pobVersion", v.pobVersion} });
		PobLaunch::SpawnPobDetached(launchLua, game);
		exitCode = 3;
		quit = true;
		cv.notify_all();
		return true;
	}

	bool start_child()
	{
		child = std::make_unique<HeadlessProc::Child>();
		HeadlessProc::Options opt;
		opt.exeDir = exeDir;
		opt.launchLua = launchLua;
		opt.game = game;
		opt.locale = locale;
		opt.hangWatch = cfg.hangWatch;
		std::string err;
		if (!child->Start(opt, err)) {
			PobLog::Error("modernui", "browser mode: headless child did not start: " + err);
			event("host.child_exited", json{ {"exitCode", -1}, {"error", err} });
			child.reset();
			return false;
		}
		child->SetLineSink([this](const std::string& line, bool isJson) {
			if (!isJson) {
				push(json{ {"event", "host.stray"}, {"data", json{ {"line", line} }} }.dump());
				return;
			}
			if (on_gate_line(line)) return;
			push(line);
		});
		child->SetExitSink([this]() {
			unsigned long code = child ? child->ExitCode() : (unsigned long)-1;
			if (file_exists(exeDir + L"pob-zh.relaunch")) {
				// POB's own "basic" self-update: Update.exe starts pob-zh.exe again,
				// which reads the marker and opens the page anew.
				event("host.updating", json{ {"exitCode", (long long)code} });
				quit = true;
				cv.notify_all();
				return;
			}
			event("host.child_exited", json{ {"exitCode", (long long)code} });
		});
		return true;
	}

	void stop_child()
	{
		if (child) {
			child->SetLineSink(nullptr);
			child->SetExitSink(nullptr);
			child->Stop(3000);
			child.reset();
		}
	}

	void host_request(const json& req)
	{
		const long long id = req.value("id", -1LL);
		const std::string method = req.value("method", "");
		const json params = req.value("params", json::object());
		auto reply = [&](const json& r) { push(json{ {"id", id}, {"result", r} }.dump()); };
		auto fail = [&](const char* code, const std::string& m) {
			push(json{ {"id", id}, {"error", json{ {"code", code}, {"message", m} }} }.dump());
		};
		if (method == "host.info") {
			reply(info());
		} else if (method == "host.close") {
			reply(json{ {"ok", true} });
			push(json{ {"event", "host.closed"} }.dump()); // every open page, not only the asker
			quit = true;
			cv.notify_all();
		} else if (method == "host.set_title") {
			reply(json{ {"ok", true} }); // the page sets document.title itself
		} else if (method == "host.open_folder") {
			std::wstring p = widen(params.value("path", ""));
			for (auto& c : p) if (c == L'/') c = L'\\';
			bool under = !pobDir.empty() && _wcsnicmp(p.c_str(), pobDir.c_str(), pobDir.size()) == 0;
			if (!under) { fail("denied", "path is outside the POB install"); return; }
			ShellExecuteW(nullptr, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			reply(json{ {"ok", true} });
		} else if (method == "host.restart_engine") {
			stop_child();
			reply(json{ {"ok", start_child()} });
		} else if (method == "host.get_prefs") {
			reply(prefs());
		} else if (method == "host.set_prefs") {
			LauncherConfig fresh = LoadLauncherConfig(exeDir + L"pob-zh.ini");
			if (params.contains("zoom")) fresh.modernZoom = ClampModernZoom(params.value("zoom", kModernZoomDefault));
			if (params.contains("fontSize")) fresh.modernFontSize = ClampModernFontSize(params.value("fontSize", kModernFontSizeDefault));
			cfg.modernZoom = fresh.modernZoom;
			cfg.modernFontSize = fresh.modernFontSize;
			SaveLauncherConfig(exeDir + L"pob-zh.ini", fresh);
			reply(prefs());
		} else {
			fail("unknown_host_method", method);
		}
	}

	void page_line(const std::string& line)
	{
		json req;
		try {
			req = json::parse(line);
		} catch (const std::exception& e) {
			push(json{ {"id", -1}, {"error", json{ {"code", "bad_json"}, {"message", e.what()} }} }.dump());
			return;
		}
		const std::string method = req.is_object() ? req.value("method", "") : "";
		if (method.rfind("host.", 0) == 0) { host_request(req); return; }
		if (!child || !child->SendRaw(line)) {
			push(json{ {"id", req.value("id", -1LL)}, {"error", json{ {"code", "child_gone"}, {"message", "the POB engine is not running"} }} }.dump());
		}
	}

	// window.pobtools for a page served over HTTP: lines go out as POSTs, come
	// back on one EventSource. The info block is baked in, as the WebView2
	// window does with its document-created script.
	std::string boot_script() const
	{
		const std::string b = prefix();
		// The EventSource resumes after a drop by itself (Last-Event-ID, see
		// serve_events); the page is told when the connection has been down for
		// a few seconds (the program ended, or was ended) and when it returns.
		return "<script>window.pobtools={"
		       "send:function(l){fetch('" + b + "~send',{method:'POST',body:String(l),keepalive:true}).catch(function(){});},"
		       "onMessage:function(fn){var es=new EventSource('" + b + "~events'),t=null,lost=false;"
		       "es.onmessage=function(e){fn(e.data);};"
		       "es.onopen=function(){if(t){clearTimeout(t);t=null;}if(lost){lost=false;fn('{\"event\":\"host.reconnected\"}');}};"
		       "es.onerror=function(){if(!t&&!lost){t=setTimeout(function(){t=null;lost=true;fn('{\"event\":\"host.disconnected\"}');},4000);}};},"
		       "info:" + info().dump() + "};</script>";
	}

	// Maps "~pob/…", "~data/…" and the page's own files to a file under their
	// root; refuses anything that climbs out.
	bool resolve(const std::string& rel, std::wstring& path, bool& isApp) const
	{
		struct Root { const char* name; std::wstring dir; };
		const Root roots[] = {
			{ "~pob/", pobDir + L"\\" }, { "~data/", exeDir + L"Data\\" }, { "~fonts/", exeDir + L"Fonts\\" },
			{ "~cache/", exeDir + L"PobTools\\cache\\" },
		};
		std::string sub = rel;
		std::wstring root = exeDir + L"ui\\";
		isApp = true;
		for (const Root& r : roots) {
			size_t n = strlen(r.name);
			if (sub.compare(0, n, r.name) == 0) { root = r.dir; sub = sub.substr(n); isApp = false; break; }
		}
		if (isApp && (sub.empty() || sub.back() == '/')) sub += "index.html";
		// no climbing, no drive letters, no backslashes smuggled in
		if (sub.find("..") != std::string::npos || sub.find(':') != std::string::npos || sub.find('\\') != std::string::npos) return false;
		std::wstring w = widen(sub);
		for (auto& c : w) if (c == L'/') c = L'\\';
		path = root + w;
		return true;
	}

	void serve_file(SOCKET s, const std::string& rel)
	{
		std::wstring path;
		bool isApp = false;
		if (!resolve(rel, path, isApp)) { respond(s, 403, "Forbidden", "text/plain", "forbidden"); return; }
		std::string body;
		if (!read_file(path, body)) { respond(s, 404, "Not Found", "text/plain", "not found"); return; }
		const char* type = mime_for(path);
		if (isApp && strncmp(type, "text/html", 9) == 0) {
			size_t at = body.find("<script");
			if (at == std::string::npos) at = body.find("</head>");
			body.insert(at == std::string::npos ? 0 : at, boot_script());
		} else if (isApp && strncmp(type, "text/css", 8) == 0) {
			// the stylesheet names the WebView2 font host; point it at ours
			const std::string from = "https://fonts.pobtools/", to = prefix() + "~fonts/";
			for (size_t p = body.find(from); p != std::string::npos; p = body.find(from, p + to.size())) body.replace(p, from.size(), to);
		}
		respond(s, 200, "OK", type, body);
	}

	void serve_events(SOCKET s, size_t resumeAfter)
	{
		const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
		                         "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
		if (!send_all(s, head)) return;
		size_t next;
		{
			std::lock_guard<std::mutex> lk(mu);
			streams++;
			everConnected = true;
			// a page that reconnects (Last-Event-ID) continues after what it has;
			// a new page gets everything still held
			next = resumeAfter == (size_t)-1 ? base : resumeAfter + 1;
		}
		for (bool stop = false;;) {
			std::vector<std::string> batch;
			{
				std::unique_lock<std::mutex> lk(mu);
				cv.wait_for(lk, std::chrono::seconds(15), [&] { return quit.load() || next < base + queue.size(); });
				stop = quit;
				if (next < base) next = base;
				if (next > base + queue.size()) next = base + queue.size();
				while (next < base + queue.size()) batch.push_back(queue[next++ - base]);
			}
			// what was queued before the end (host.closed) still goes out
			if (stop && batch.empty()) break;
			std::string out;
			if (batch.empty()) out = ": keep-alive\n\n";
			size_t id = next - batch.size();
			for (const std::string& l : batch) out += "id: " + std::to_string(id++) + "\ndata: " + l + "\n\n";
			if (!send_all(s, out) || stop) break;
		}
		std::lock_guard<std::mutex> lk(mu);
		streams--;
		lastStreamSeen = std::chrono::steady_clock::now();
	}

	void handle(SOCKET s)
	{
		// request line + headers
		std::string req;
		char buf[8192];
		size_t headerEnd = std::string::npos;
		while (headerEnd == std::string::npos && req.size() < 65536) {
			int n = recv(s, buf, sizeof(buf), 0);
			if (n <= 0) return;
			req.append(buf, n);
			headerEnd = req.find("\r\n\r\n");
		}
		if (headerEnd == std::string::npos) return;
		size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
		if (sp2 == std::string::npos) return;
		const std::string method = req.substr(0, sp1);
		std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
		size_t q = target.find('?');
		if (q != std::string::npos) target = target.substr(0, q);
		target = url_decode(target);
		// Host header must be our loopback address: a page on some other site
		// pointing a DNS name at 127.0.0.1 cannot reach us (DNS rebinding).
		{
			// (+2 keeps the last header line's CRLF, so Host may come last)
			std::string lower = req.substr(0, headerEnd + 2);
			for (auto& c : lower) c = (char)tolower((unsigned char)c);
			const std::string want = "\r\nhost: 127.0.0.1:" + std::to_string(port) + "\r\n";
			if (lower.find(want) == std::string::npos) { respond(s, 421, "Misdirected Request", "text/plain", "bad host"); return; }
		}
		const std::string pre = prefix();
		if (target.compare(0, pre.size(), pre) != 0) { respond(s, 404, "Not Found", "text/plain", "not found"); return; }
		const std::string rel = target.substr(pre.size());
		if (method == "GET" && rel == "~events") {
			size_t resume = (size_t)-1;
			std::string lower = req.substr(0, headerEnd + 2);
			for (auto& c : lower) c = (char)tolower((unsigned char)c);
			size_t at = lower.find("\r\nlast-event-id:");
			if (at != std::string::npos) resume = (size_t)strtoull(lower.c_str() + at + 16, nullptr, 10);
			serve_events(s, resume);
			return;
		}
		if (method == "POST" && rel == "~send") {
			size_t len = 0;
			{
				std::string lower = req.substr(0, headerEnd);
				for (auto& c : lower) c = (char)tolower((unsigned char)c);
				size_t at = lower.find("\r\ncontent-length:");
				if (at != std::string::npos) len = (size_t)strtoull(lower.c_str() + at + 17, nullptr, 10);
			}
			if (len > 16 * 1024 * 1024) { respond(s, 413, "Payload Too Large", "text/plain", "too large"); return; }
			std::string body = req.substr(headerEnd + 4);
			while (body.size() < len) {
				int n = recv(s, buf, sizeof(buf), 0);
				if (n <= 0) return;
				body.append(buf, n);
			}
			body.resize(len);
			respond(s, 204, "No Content", "text/plain", "");
			page_line(body);
			return;
		}
		if (method == "GET") { serve_file(s, rel); return; }
		respond(s, 405, "Method Not Allowed", "text/plain", "method not allowed");
	}

	bool listen_loopback()
	{
		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET) return false;
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
		if (listen(listener, SOMAXCONN) != 0) return false;
		int alen = sizeof(addr);
		getsockname(listener, (sockaddr*)&addr, &alen);
		port = ntohs(addr.sin_port);
		return port != 0;
	}

	void accept_loop()
	{
		while (!quit) {
			fd_set rd;
			FD_ZERO(&rd);
			FD_SET(listener, &rd);
			timeval tv{ 0, 500000 };
			if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
			SOCKET c = accept(listener, nullptr, nullptr);
			if (c == INVALID_SOCKET) continue;
			std::thread([this, c]() {
				handle(c);
				shutdown(c, SD_BOTH);
				closesocket(c);
			}).detach();
		}
	}
};

std::wstring UrlFile(const std::wstring& exeDir, const std::wstring& game)
{
	return exeDir + L"PobTools\\modern_ui_url_" + (game == L"poe2" ? std::wstring(L"poe2") : std::wstring(L"poe1")) + L".txt";
}

// One short request to our own loopback server: status code, or -1.
int LoopbackRequest(const std::string& url, const char* method, const std::string& body)
{
	if (url.rfind("http://127.0.0.1:", 0) != 0) return -1;
	const int port = atoi(url.c_str() + 17);
	const size_t slash = url.find('/', 17);
	if (port <= 0 || slash == std::string::npos) return -1;
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
	int status = -1;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s != INVALID_SOCKET) {
		DWORD tmo = 1500;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons((u_short)port);
		if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
			const std::string req = std::string(method) + " " + url.substr(slash) + " HTTP/1.1\r\nHost: 127.0.0.1:" +
			                        std::to_string(port) + "\r\nContent-Length: " + std::to_string(body.size()) +
			                        "\r\nConnection: close\r\n\r\n" + body;
			if (send_all(s, req)) {
				char buf[64] = {};
				const int n = recv(s, buf, sizeof(buf) - 1, 0);
				if (n > 12 && strncmp(buf, "HTTP/1.1 ", 9) == 0) status = atoi(buf + 9);
			}
		}
		closesocket(s);
	}
	WSACleanup();
	return status;
}

} // namespace

bool ModernUiBrowserRunning(const std::wstring& exeDir, const std::wstring& game, std::string* url)
{
	std::string u;
	if (!read_file(UrlFile(exeDir, game), u) || u.empty()) return false;
	if (LoopbackRequest(u, "GET", "") != 200) {
		// the program ended without cleaning up (killed): the file is stale
		DeleteFileW(UrlFile(exeDir, game).c_str());
		return false;
	}
	if (url) *url = u;
	return true;
}

bool ModernUiBrowserOpen(const std::wstring& exeDir, const std::wstring& game)
{
	std::string u;
	if (!ModernUiBrowserRunning(exeDir, game, &u)) return false;
	ShellExecuteW(nullptr, L"open", widen(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return true;
}

bool ModernUiBrowserStop(const std::wstring& exeDir, const std::wstring& game)
{
	std::string u;
	if (!ModernUiBrowserRunning(exeDir, game, &u)) return false;
	return LoopbackRequest(u + "~send", "POST", "{\"id\":-1,\"method\":\"host.close\"}") == 204;
}

bool ModernUiBrowserAvailable(const std::wstring& exeDir)
{
	return file_exists(exeDir + L"ui\\index.html");
}

int ShowModernUiInBrowser(const std::wstring& exeDir, const std::wstring& game,
                          const std::wstring& locale, const LauncherConfig& cfg,
                          const std::wstring& openBuild, const std::wstring& pobDirOverride)
{
	auto srv = std::make_unique<Server>();
	Server& S = *srv;
	S.exeDir = exeDir;
	S.game = game;
	S.locale = locale;
	S.cfg = cfg;
	S.openBuild = openBuild;
	const InstallInfo installs = DetectInstalls(exeDir);
	const bool poe2 = game == L"poe2";
	S.pobDir = poe2 ? installs.poe2Dir : installs.poe1Dir;
	S.launchLua = poe2 ? installs.poe2Lua : installs.poe1Lua;
	if (!pobDirOverride.empty()) {
		S.pobDir = pobDirOverride;
		S.launchLua = pobDirOverride + L"\\Launch.lua";
	}
	if (!ModernUiBrowserAvailable(exeDir)) {
		PobLog::Error("modernui", "browser mode: ui\\index.html is missing");
		return 1;
	}
	if (S.pobDir.empty() || !file_exists(S.launchLua)) {
		PobLog::Error("modernui", "browser mode: no POB install found for " + narrow(game));
		MessageBoxW(nullptr, L"找不到 Path of Building 安裝資料夾(Launch.lua)。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	CreateDirectoryW((exeDir + L"PobTools\\cache").c_str(), nullptr);
	wchar_t noBrowser[8] = {};
	const bool openBrowser = !GetEnvironmentVariableW(L"POB_ZH_NO_BROWSER", noBrowser, 8);
	// One per game: launching again while it runs opens the running one's page
	// instead of a second engine on the same install (they would overwrite
	// each other's Settings.xml).
	{
		std::string running;
		if (ModernUiBrowserRunning(exeDir, game, &running)) {
			if (openBrowser) ShellExecuteW(nullptr, L"open", widen(running).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return 0;
		}
	}
	S.token = random_token();
	if (!S.listen_loopback()) {
		PobLog::Error("modernui", "browser mode: cannot listen on 127.0.0.1, WSA error " + std::to_string(WSAGetLastError()));
		return 2;
	}
	S.start_child();
	std::thread acceptor([&S]() { S.accept_loop(); });

	const std::string url = S.origin() + S.prefix();
	// Where to find it again (and what a test harness reads): the URL is only
	// good for this run, and only from this machine.
	{
		HANDLE h = CreateFileW(UrlFile(exeDir, game).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			WriteFile(h, url.data(), (DWORD)url.size(), &w, nullptr);
			CloseHandle(h);
		}
	}
	if (openBrowser) {
		// Wine hands an http URL to winebrowser, which opens the host system's
		// browser (xdg-open on Linux, open on macOS). Under CrossOver that can
		// fail (no browser bottle association); say where the page is instead
		// of leaving the user with nothing to click.
		HINSTANCE rc = ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		if ((INT_PTR)rc <= 32) {
			PobLog::Error("modernui", "browser mode: could not open a browser (code " + std::to_string((INT_PTR)rc) +
			                              "); open this address yourself: " + url);
			printf("%s\n", url.c_str());
			fflush(stdout);
		}
	}

	const auto started = std::chrono::steady_clock::now();
	while (!S.quit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		std::lock_guard<std::mutex> lk(S.mu);
		const auto now = std::chrono::steady_clock::now();
		if (!S.everConnected && now - started > std::chrono::seconds(kFirstConnectSeconds)) {
			PobLog::Error("modernui", "browser mode: the page never connected; stopping");
			break;
		}
		if (S.everConnected && S.streams == 0 && now - S.lastStreamSeen > std::chrono::seconds(kGoneSeconds)) break;
	}
	S.quit = true;
	S.cv.notify_all();
	S.stop_child();
	closesocket(S.listener);
	acceptor.join();
	// give event-stream threads a moment to see `quit`
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	DeleteFileW(UrlFile(exeDir, game).c_str());
	const int code = S.exitCode;
	// Connection threads are detached and may still be unwinding; the process
	// is about to exit, so the server object is left for them rather than
	// freed underneath them.
	srv.release();
	return code;
}
