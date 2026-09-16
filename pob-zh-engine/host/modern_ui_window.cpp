#include "modern_ui_window.h"

#include "app_version.h"
#include "error_log.h"
#include "headless_proc.h"
#include "launcher_config.h"
#include "pob_launch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>

#include <json.hpp>

#include <memory>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {

constexpr wchar_t kClassName[] = L"PobToolsModernUi";
constexpr UINT WM_CHILD_LINE = WM_APP + 1;   // lParam = new std::string (a line from the child)
constexpr UINT WM_CHILD_EXIT = WM_APP + 2;
constexpr wchar_t kHostApp[] = L"app.pobtools";
constexpr wchar_t kHostPob[] = L"pob.pobtools";
constexpr wchar_t kHostData[] = L"data.pobtools";
constexpr wchar_t kHostFonts[] = L"fonts.pobtools";

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

// Per-monitor-v2 without a manifest: the launcher gets it from glfwInit, this
// process never calls GLFW. Resolved at run time so Windows 8/early 10 still start.
void EnableDpiAwareness()
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (!user32) return;
	using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
	auto fn = (Fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
	if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

UINT DpiFor(HWND hwnd)
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	using Fn = UINT(WINAPI*)(HWND);
	auto fn = user32 ? (Fn)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
	UINT dpi = fn && hwnd ? fn(hwnd) : 0;
	return dpi ? dpi : 96;
}

struct Window {
	std::wstring exeDir, game, locale;
	LauncherConfig cfg;
	HWND hwnd = nullptr;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> webview;
	std::unique_ptr<HeadlessProc::Child> child;
	InstallInfo installs;
	std::wstring pobDir, launchLua;
	bool pageReady = false;
	// Lines from the child that arrived before the page could take them
	// (WebView2 creation is asynchronous; the child is started first so its
	// boot overlaps). Flushed on navigation completed.
	std::vector<std::string> backlog;
	int exitCode = 0;

	void PostToPage(const std::string& jsonLine)
	{
		if (!webview || !pageReady) {
			backlog.push_back(jsonLine);
			return;
		}
		webview->PostWebMessageAsJson(widen(jsonLine).c_str());
	}

	void PostEvent(const char* name, const json& data)
	{
		PostToPage(json{ {"event", name}, {"data", data} }.dump());
	}

	bool StartChild()
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
			PobLog::Error("modernui", "headless child did not start: " + err);
			PostEvent("host.child_exited", json{ {"exitCode", -1}, {"error", err} });
			child.reset();
			return false;
		}
		HWND h = hwnd;
		child->SetLineSink([h](const std::string& line, bool isJson) {
			std::string* payload;
			if (isJson) {
				payload = new std::string(line);
			} else {
				// A stray print from POB's Lua (or a crash dump): still worth showing.
				payload = new std::string(json{ {"event", "host.stray"}, {"data", json{ {"line", line} }} }.dump());
			}
			if (!PostMessageW(h, WM_CHILD_LINE, 0, (LPARAM)payload)) delete payload;
		});
		child->SetExitSink([h]() { PostMessageW(h, WM_CHILD_EXIT, 0, 0); });
		return true;
	}

	void StopChild()
	{
		if (child) {
			child->SetLineSink(nullptr);
			child->SetExitSink(nullptr);
			child->Stop(3000);
			child.reset();
		}
	}

	json Info() const
	{
		return json{
			{"game", narrow(game)},
			{"locale", narrow(locale)},
			{"exeDir", narrow(exeDir)},
			{"pobDir", narrow(pobDir)},
			{"version", POBTOOLS_VERSION_STRING},
			{"hosts", json{ {"app", narrow(kHostApp)}, {"pob", narrow(kHostPob)},
			                {"data", narrow(kHostData)}, {"fonts", narrow(kHostFonts)} }},
		};
	}

	// The page asked the host itself for something. Anything not understood is
	// answered with an error rather than silently forwarded to the child, where
	// the bridge would report "unknown method" and the page could not tell the
	// two apart.
	void HandleHostRequest(const json& req)
	{
		const long long id = req.value("id", -1LL);
		const std::string method = req.value("method", "");
		const json params = req.value("params", json::object());
		auto reply = [&](const json& result) {
			PostToPage(json{ {"id", id}, {"result", result} }.dump());
		};
		auto fail = [&](const char* code, const std::string& msg) {
			PostToPage(json{ {"id", id}, {"error", json{ {"code", code}, {"message", msg} }} }.dump());
		};
		if (method == "host.info") {
			reply(Info());
		} else if (method == "host.close") {
			reply(json{ {"ok", true} });
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		} else if (method == "host.set_title") {
			std::wstring t = widen(params.value("text", ""));
			SetWindowTextW(hwnd, t.empty() ? L"PobTools" : (t + L" - PobTools").c_str());
			reply(json{ {"ok", true} });
		} else if (method == "host.open_folder") {
			// Only folders under the POB install: the page is ours, but the rule
			// costs nothing and keeps "open a folder" from becoming "open anything".
			std::wstring p = widen(params.value("path", ""));
			for (auto& c : p) if (c == L'/') c = L'\\';
			std::wstring root = pobDir;
			bool under = !root.empty() && _wcsnicmp(p.c_str(), root.c_str(), root.size()) == 0;
			if (!under) { fail("denied", "path is outside the POB install"); return; }
			ShellExecuteW(nullptr, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			reply(json{ {"ok", true} });
		} else if (method == "host.restart_engine") {
			StopChild();
			bool ok = StartChild();
			reply(json{ {"ok", ok} });
		} else {
			fail("unknown_host_method", method);
		}
	}

	void OnWebMessage(const std::wstring& msg)
	{
		std::string line = narrow(msg);
		// Cheap routing: parse only to find the method. The child gets the
		// original text, untouched.
		json req;
		try {
			req = json::parse(line);
		} catch (const std::exception& e) {
			PostToPage(json{ {"id", -1}, {"error", json{ {"code", "bad_json"}, {"message", e.what()} }} }.dump());
			return;
		}
		std::string method = req.is_object() ? req.value("method", "") : "";
		if (method.rfind("host.", 0) == 0) {
			HandleHostRequest(req);
			return;
		}
		if (!child || !child->SendRaw(line)) {
			PostToPage(json{ {"id", req.value("id", -1LL)},
			                 {"error", json{ {"code", "child_gone"}, {"message", "the POB engine is not running"} }} }.dump());
		}
	}

	std::wstring BootScript() const
	{
		// The page's only door to the outside. `info` is baked in so the page
		// knows game/locale before its first round trip.
		std::string info = Info().dump();
		return widen(
			"window.pobtools = {"
			"  send: function(line) { window.chrome.webview.postMessage(String(line)); },"
			"  onMessage: function(fn) { window.chrome.webview.addEventListener('message', function(e) { fn(e.data); }); },"
			"  info: " + info +
			"};");
	}

	HRESULT OnControllerCreated(HRESULT hr, ICoreWebView2Controller* ctl)
	{
		if (FAILED(hr) || !ctl) {
			PobLog::Error("modernui", "WebView2 controller creation failed, hr=" + std::to_string((long)hr));
			MessageBoxW(hwnd, L"WebView2 無法建立(詳見 PobTools\\logs)。", L"PobTools", MB_ICONERROR | MB_OK);
			exitCode = 2;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			return hr;
		}
		controller = ctl;
		controller->get_CoreWebView2(&webview);
		if (!webview) return E_FAIL;

		ComPtr<ICoreWebView2Settings> settings;
		webview->get_Settings(&settings);
		if (settings) {
			settings->put_IsStatusBarEnabled(FALSE);
			settings->put_AreDefaultContextMenusEnabled(FALSE);
			settings->put_AreHostObjectsAllowed(FALSE);
			settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
			settings->put_AreDevToolsEnabled(FALSE);
#endif
		}

		ComPtr<ICoreWebView2_3> wv3;
		if (FAILED(webview.As(&wv3)) || !wv3) {
			PobLog::Error("modernui", "WebView2 runtime too old: ICoreWebView2_3 (virtual host mapping) missing");
			MessageBoxW(hwnd, L"WebView2 Runtime 版本太舊,請更新 Microsoft Edge WebView2。", L"PobTools", MB_ICONERROR | MB_OK);
			exitCode = 2;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			return E_FAIL;
		}
		// The page may load its own files freely; the POB install, the data and
		// font folders are readable but not cross-origin (the page is the only
		// origin that should see them anyway).
		wv3->SetVirtualHostNameToFolderMapping(kHostApp, (exeDir + L"ui").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
		if (!pobDir.empty())
			wv3->SetVirtualHostNameToFolderMapping(kHostPob, pobDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
		wv3->SetVirtualHostNameToFolderMapping(kHostData, (exeDir + L"Data").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
		wv3->SetVirtualHostNameToFolderMapping(kHostFonts, (exeDir + L"Fonts").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);

		webview->AddScriptToExecuteOnDocumentCreated(BootScript().c_str(), nullptr);

		EventRegistrationToken tok{};
		webview->add_WebMessageReceived(
			Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
					LPWSTR s = nullptr;
					if (SUCCEEDED(args->TryGetWebMessageAsString(&s)) && s) {
						OnWebMessage(s);
						CoTaskMemFree(s);
					}
					return S_OK;
				}).Get(), &tok);
		webview->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
					BOOL ok = FALSE;
					args->get_IsSuccess(&ok);
					if (!ok) {
						COREWEBVIEW2_WEB_ERROR_STATUS st{};
						args->get_WebErrorStatus(&st);
						PobLog::Error("modernui", "page navigation failed, status=" + std::to_string((int)st));
					}
					pageReady = true;
					for (auto& l : backlog) webview->PostWebMessageAsJson(widen(l).c_str());
					backlog.clear();
					return S_OK;
				}).Get(), &tok);

		RECT rc{};
		GetClientRect(hwnd, &rc);
		controller->put_Bounds(rc);
		webview->Navigate((std::wstring(L"https://") + kHostApp + L"/index.html").c_str());
		return S_OK;
	}

	HRESULT OnEnvironmentCreated(HRESULT hr, ICoreWebView2Environment* env)
	{
		if (FAILED(hr) || !env) {
			PobLog::Error("modernui", "WebView2 environment creation failed, hr=" + std::to_string((long)hr));
			MessageBoxW(hwnd, L"WebView2 環境無法建立(詳見 PobTools\\logs)。", L"PobTools", MB_ICONERROR | MB_OK);
			exitCode = 2;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			return hr;
		}
		return env->CreateCoreWebView2Controller(hwnd,
			Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
				[this](HRESULT h, ICoreWebView2Controller* c) { return OnControllerCreated(h, c); }).Get());
	}

	void SaveWindowSize()
	{
		RECT rc{};
		if (!GetWindowRect(hwnd, &rc) || IsIconic(hwnd) || IsZoomed(hwnd)) return;
		// Re-read: the launcher may have written other keys since we started.
		LauncherConfig fresh = LoadLauncherConfig(exeDir + L"pob-zh.ini");
		fresh.modernWinW = ClampWindowDim(rc.right - rc.left);
		fresh.modernWinH = ClampWindowDim(rc.bottom - rc.top);
		if (fresh.modernWinW == 0 || fresh.modernWinH == 0) return;
		SaveLauncherConfig(exeDir + L"pob-zh.ini", fresh);
	}
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Window* w = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_CHILD_LINE: {
		std::unique_ptr<std::string> line((std::string*)lParam);
		if (w && line) w->PostToPage(*line);
		return 0;
	}
	case WM_CHILD_EXIT:
		if (w) {
			unsigned long code = w->child ? w->child->ExitCode() : (unsigned long)-1;
			PobLog::Error("modernui", "headless child exited, code=" + std::to_string((long)code));
			w->PostEvent("host.child_exited", json{ {"exitCode", (long long)code} });
		}
		return 0;
	case WM_SIZE:
		if (w && w->controller) {
			RECT rc{};
			GetClientRect(hwnd, &rc);
			w->controller->put_Bounds(rc);
		}
		return 0;
	case WM_DPICHANGED: {
		const RECT* r = (const RECT*)lParam;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_CLOSE:
		if (w) {
			w->SaveWindowSize();
			w->StopChild();
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool ModernUiAvailable(const std::wstring& exeDir, std::wstring* why)
{
	auto no = [&](const wchar_t* reason) { if (why) *why = reason; return false; };
	if (PobLaunch::RunningUnderWine()) return no(L"WebView2 does not run under Wine");
	if (!file_exists(exeDir + L"engine\\WebView2Loader.dll")) return no(L"engine\\WebView2Loader.dll is missing");
	if (!file_exists(exeDir + L"ui\\index.html")) return no(L"ui\\index.html is missing (run `npm run build` in ui/)");
	// Loader present: the delay-load below resolves from engine\ (SetDllDirectoryW
	// in wWinMain). The function answers whether an Evergreen Runtime is installed.
	LPWSTR ver = nullptr;
	HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
	if (FAILED(hr) || !ver) return no(L"WebView2 Runtime is not installed");
	CoTaskMemFree(ver);
	return true;
}

int ShowModernUi(const std::wstring& exeDir, const std::wstring& game,
                 const std::wstring& locale, const LauncherConfig& cfg)
{
	std::wstring why;
	if (!ModernUiAvailable(exeDir, &why)) {
		PobLog::Error("modernui", "cannot open: " + narrow(why));
		MessageBoxW(nullptr,
			(L"無法開啟新介面:" + why + L"\n\nWebView2 Runtime 下載:\nhttps://developer.microsoft.com/microsoft-edge/webview2/").c_str(),
			L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}

	Window w;
	w.exeDir = exeDir;
	w.game = game;
	w.locale = locale;
	w.cfg = cfg;
	w.installs = DetectInstalls(exeDir);
	const bool poe2 = (game == L"poe2");
	w.pobDir = poe2 ? w.installs.poe2Dir : w.installs.poe1Dir;
	w.launchLua = poe2 ? w.installs.poe2Lua : w.installs.poe1Lua;
	if (w.pobDir.empty() || !file_exists(w.launchLua)) {
		PobLog::Error("modernui", "no POB install found for " + narrow(game));
		MessageBoxW(nullptr, L"找不到 Path of Building 安裝資料夾(Launch.lua)。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}

	EnableDpiAwareness();
	// STA for WebView2's COM callbacks; S_FALSE (already initialised) is fine.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	HINSTANCE hInst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = kClassName;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(20, 20, 24));
	wc.hIcon = LoadIconW(hInst, L"GLFW_ICON"); // the app icon (host/pob-zh.rc); the name is historical
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);

	// Size: remembered pair, else 1500x950 scaled to the primary monitor's DPI.
	int width = cfg.modernWinW, height = cfg.modernWinH;
	if (width == 0 || height == 0) {
		HDC dc = GetDC(nullptr);
		const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
		if (dc) ReleaseDC(nullptr, dc);
		width = MulDiv(1500, dpi, 96);
		height = MulDiv(950, dpi, 96);
	}
	w.hwnd = CreateWindowExW(0, kClassName, L"PobTools", WS_OVERLAPPEDWINDOW,
	                         CW_USEDEFAULT, CW_USEDEFAULT, width, height,
	                         nullptr, nullptr, hInst, nullptr);
	if (!w.hwnd) {
		PobLog::Error("modernui", "CreateWindowExW failed, GetLastError=" + std::to_string((unsigned long)GetLastError()));
		return 1;
	}
	SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, (LONG_PTR)&w);
	ShowWindow(w.hwnd, SW_SHOWNORMAL);

	// The child boots (~2-3 s with dictionaries) while WebView2 initialises;
	// anything it says before the page is up waits in the backlog.
	w.StartChild();

	const std::wstring userData = exeDir + L"PobTools\\webview2";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	CreateDirectoryW(userData.c_str(), nullptr);
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[&w](HRESULT h, ICoreWebView2Environment* env) { return w.OnEnvironmentCreated(h, env); }).Get());
	if (FAILED(hr)) {
		PobLog::Error("modernui", "CreateCoreWebView2EnvironmentWithOptions failed, hr=" + std::to_string((long)hr));
		MessageBoxW(w.hwnd, L"WebView2 無法啟動(詳見 PobTools\\logs)。", L"PobTools", MB_ICONERROR | MB_OK);
		w.StopChild();
		DestroyWindow(w.hwnd);
		return 2;
	}

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	w.StopChild();
	w.webview.Reset();
	w.controller.Reset();
	return w.exitCode;
}
