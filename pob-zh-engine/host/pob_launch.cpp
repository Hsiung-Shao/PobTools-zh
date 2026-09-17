#include "pob_launch.h"
#include "error_log.h"
#include "../translate/startup_trace.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace PobLaunch {
namespace {

struct Instance {
	HANDLE handle;
	std::wstring game;
	InstanceKind kind = InstanceKind::Pob;
	DWORD pid = 0;
	HWND hwnd = nullptr; // resolved on demand; see resolve_main_window
};
std::vector<Instance> g_instances;

// Drop every finished child. Split out of PobRunningCount because three callers
// now need "make the table current" without wanting POB's count.
void reap()
{
	for (size_t i = 0; i < g_instances.size();) {
		if (WaitForSingleObject(g_instances[i].handle, 0) == WAIT_OBJECT_0) {
			CloseHandle(g_instances[i].handle);
			g_instances.erase(g_instances.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
}

// The engine's main window is the one GLFW made; its class name starts with
// "GLFW" (GLFW30 today). Everything else a POB process owns -- the engine's
// console window (sys_console.cpp registers "SimpleGraphic Console Class"), the
// IME and GLFW helper windows -- is not it.
bool is_glfw_window(HWND hwnd)
{
	wchar_t cls[64] = {};
	GetClassNameW(hwnd, cls, 64);
	return wcsncmp(cls, L"GLFW", 4) == 0;
}

struct FindWindowCtx {
	DWORD pid;
	HWND  found; // the GLFW window, or null
};

// The main window of a child process, or null while it has none yet.
//
// GLFW creates the window HIDDEN and shows it later (sys_video.cpp passes
// GLFW_VISIBLE=FALSE so the user never sees an unpositioned stock window), so
// "not found" is the normal answer for the first moments of a child's life and
// must not be cached as "this one has no window".
//
// Only the GLFW class counts. There used to be a fallback to "the first visible
// unowned window of the pid", meant as insurance; what it actually caught was
// the engine's console window, which is created VISIBLE and stays the only
// visible window of a fresh POB for its first ~0.3 s (the GLFW window is shown
// during init, the console is hidden only after init). The launcher resolves
// every 0.5 s, so about every other POB start latched onto the console, and
// every appearance message and watchdog probe went to a window that ignored
// them -- "the sliders do nothing until I restart POB a few times"
// (2026-09-12). Nothing needs the fallback: a POB without a GLFW window is a
// POB that has not finished starting.
BOOL CALLBACK find_window_cb(HWND hwnd, LPARAM param)
{
	auto* ctx = (FindWindowCtx*)param;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ctx->pid) return TRUE;
	if (!IsWindowVisible(hwnd)) return TRUE;
	if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
	if (!is_glfw_window(hwnd)) return TRUE;
	ctx->found = hwnd;
	return FALSE; // found it, stop enumerating
}

HWND resolve_main_window(DWORD pid)
{
	FindWindowCtx ctx{ pid, nullptr };
	EnumWindows(find_window_cb, (LPARAM)&ctx);
	return ctx.found;
}

std::wstring exe_path()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

// Set an environment variable in BOTH the Win32 and CRT environments.
// This exe uses the STATIC CRT, so the engine's getenv() reads a different
// (UCRT) environment: ucrtbase.dll initialises when the first /MD DLL loads
// and snapshots the Win32 environment AT THAT MOMENT. SetEnvironmentVariableW
// is therefore the critical half here — it runs before any DLL is loaded.
// Do not remove either call.
void set_env_both(const wchar_t* var, const wchar_t* val)
{
	SetEnvironmentVariableW(var, val);
	_wputenv_s(var, val);
}

// Same identity rule the update lock uses (app_update.cpp): FNV-1a over the
// lowercased install directory, so "is a POB running" and "is an update
// swapping files" agree on what counts as the same installation.
std::wstring marker_name(const std::wstring& exeDir)
{
	unsigned long long h = 1469598103934665603ull;
	for (wchar_t c : exeDir) {
		wchar_t lc = (wchar_t)towlower(c);
		h ^= (unsigned long long)lc;
		h *= 1099511628211ull;
	}
	wchar_t buf[64];
	swprintf(buf, 64, L"Local\\PobTools-engine-%016llx", h);
	return buf;
}

bool spawn(const std::wstring& launchLua, PROCESS_INFORMATION& pi)
{
	std::wstring exe = exe_path();
	std::wstring cmd = L"\"" + exe + L"\" --engine \"" + launchLua + L"\"";
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	pi = PROCESS_INFORMATION{};
	if (CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0,
	                   nullptr, nullptr, &si, &pi))
		return true;
	// POB is started as `pob-zh.exe --engine <Launch.lua>`, NOT by running POB's
	// own exe -- so the thing that fails here is spawning ourselves again, and the
	// usual cause (anti-virus holding the exe, the install moved) is only in
	// GetLastError, which is gone by the time anyone asks.
	PobLog::Error("pob", "CreateProcess for the POB child failed, GetLastError=" +
	                         std::to_string((unsigned long)GetLastError()));
	MessageBoxW(nullptr, L"無法啟動 POB 子程序。", L"PobTools", MB_ICONERROR | MB_OK);
	return false;
}

} // namespace

bool RunningUnderWine()
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// Mirrored in engine/system/win/sys_video.cpp (kMsgSetWindowOpacity): the engine
// subclasses its GLFW window and applies the percent itself -- the opacity is
// per-pixel (background only), so it is the renderer's job, not a window style.
static const UINT kMsgSetWindowOpacity = WM_APP + 0x50;

void ApplyPobWindowOpacity(const std::wstring& game, int percent)
{
	if (RunningUnderWine()) return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	for (const InstanceInfo& in : RunningInstances()) {
		if (in.kind != InstanceKind::Pob || !in.hwnd || in.game != game) continue;
		PostMessageW((HWND)in.hwnd, kMsgSetWindowOpacity, (WPARAM)percent, 0);
	}
}

static std::string to_utf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], needed, nullptr, nullptr);
	return s;
}

static const UINT      kMsgSetGlassBlur   = WM_APP + 0x51; // mirrored in engine sys_video.cpp
static const UINT      kMsgSetBgBright    = WM_APP + 0x52;
static const UINT      kMsgSetTreeBg      = WM_APP + 0x53;
static const ULONG_PTR kCopyDataBgPath    = 0x50;

static void post_pct(const std::wstring& game, UINT msg, int percent)
{
	if (RunningUnderWine()) return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	for (const InstanceInfo& in : RunningInstances()) {
		if (in.kind != InstanceKind::Pob || !in.hwnd || in.game != game) continue;
		PostMessageW((HWND)in.hwnd, msg, (WPARAM)percent, 0);
	}
}

void ApplyPobBackgroundBright(const std::wstring& game, int percent) { post_pct(game, kMsgSetBgBright, percent); }

static const UINT kMsgSetFrameCap = WM_APP + 0x54; // mirrored in engine sys_video.cpp

void ApplyPobFrameCap(int fpsForeground, int fpsBackground)
{
	if (RunningUnderWine()) return;
	for (const InstanceInfo& in : RunningInstances()) {
		if (in.kind != InstanceKind::Pob || !in.hwnd) continue;
		PostMessageW((HWND)in.hwnd, kMsgSetFrameCap, (WPARAM)fpsForeground, (LPARAM)fpsBackground);
	}
}
void ApplyPobGlassBlur(const std::wstring& game, int percent)        { post_pct(game, kMsgSetGlassBlur, percent); }
void ApplyPobTreeBackdrop(const std::wstring& game, int percent)     { post_pct(game, kMsgSetTreeBg, percent); }

void ApplyPobBackground(const std::wstring& game, const std::wstring& bgPath)
{
	if (RunningUnderWine()) return;
	std::string utf8 = to_utf8(bgPath);
	COPYDATASTRUCT cds{};
	cds.dwData = kCopyDataBgPath;
	cds.cbData = (DWORD)utf8.size();
	cds.lpData = utf8.empty() ? (void*)"" : (void*)utf8.data();
	for (const InstanceInfo& in : RunningInstances()) {
		if (in.kind != InstanceKind::Pob || !in.hwnd || in.game != game) continue;
		// SendMessageTimeout: WM_COPYDATA must be sent, not posted; never hang on
		// a POB that is busy.
		DWORD_PTR res = 0;
		SendMessageTimeoutW((HWND)in.hwnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 500, &res);
	}
}

void SetEngineEnv(const std::wstring& game, const std::wstring& locale,
                  const std::wstring& fontFile, const std::wstring& dataDir,
                  bool fontApplyAll, int windowOpacity,
                  const std::wstring& bgPath, int bgBright, int glassBlur, int treeBg,
                  bool hangWatch, int fpsForeground, int fpsBackground, bool perfLog)
{
	// "0" only when the user opted out in the ini; the engine's watchdog reads it
	// before it starts its thread. Always written for the same long-lived-process
	// reason as the variables below.
	set_env_both(L"POB_ZH_HANGWATCH", hangWatch ? L"1" : L"0");
	set_env_both(L"POB_ZH_BG", bgPath.c_str());
	set_env_both(L"POB_ZH_BG_BRIGHT", std::to_wstring(bgBright).c_str());
	set_env_both(L"POB_ZH_GLASS_BLUR", std::to_wstring(glassBlur).c_str());
	set_env_both(L"POB_ZH_TREE_BG", std::to_wstring(treeBg).c_str());
	set_env_both(L"POB_GAME", game.c_str());
	set_env_both(L"POB_LOCALE", locale.c_str());
	set_env_both(L"POB_ZH_FONTFILE", fontFile.c_str()); // r_font.cpp reads this
	// Always written, empty included: this process is long-lived in KeepOpen mode,
	// so clearing the setting has to actually clear the variable rather than leave
	// the previous launch's value behind for the next POB to inherit.
	set_env_both(L"POB_ZH_DATADIR", dataDir.c_str());
	// "0"/"1", always written for the same long-lived-process reason as above.
	set_env_both(L"POB_ZH_FONT_ALL", fontApplyAll ? L"1" : L"0");
	// Percent, always written; sys_video.cpp reads it right after glfwCreateWindow.
	set_env_both(L"POB_ZH_WINDOW_OPACITY", std::to_wstring(windowOpacity).c_str());
	// Frame caps (fps, 0 = none) and the opt-in performance log; always written,
	// same long-lived-process reason as above.
	set_env_both(L"POB_ZH_FPS_FG", std::to_wstring(fpsForeground).c_str());
	set_env_both(L"POB_ZH_FPS_BG", std::to_wstring(fpsBackground).c_str());
	set_env_both(L"POB_ZH_PERFLOG", perfLog ? L"1" : L"0");
}

unsigned long SpawnPobAndWait(const std::wstring& launchLua)
{
	PROCESS_INFORMATION pi{};
	if (!spawn(launchLua, pi)) return (unsigned long)-1;
	CloseHandle(pi.hThread);
	// Both ends of the wait go into the startup timeline: "closed POB, launcher
	// came back seconds later" is only diagnosable when the child's exit and the
	// launcher's return are on the same clock.
	startup_trace_mark("POB child spawned (pid %lu); waiting for it to exit", (unsigned long)pi.dwProcessId);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	startup_trace_mark("POB child exited (code %lu)", (unsigned long)code);
	return code;
}

bool SpawnPobDetached(const std::wstring& launchLua, const std::wstring& game,
                      unsigned long* outPid)
{
	PROCESS_INFORMATION pi{};
	if (!spawn(launchLua, pi)) return false;
	CloseHandle(pi.hThread);
	// handle closed when it is reaped
	g_instances.push_back({ pi.hProcess, game, InstanceKind::Pob, pi.dwProcessId, nullptr });
	if (outPid) *outPid = pi.dwProcessId;
	return true;
}

// The launcher's own environment minus the variables SetEngineEnv writes. Tools
// read pob-zh.ini, but LoadLauncherConfig lets POB_GAME / POB_LOCALE override the
// ini -- so a tool started after a POB had run would inherit THAT launch's game
// and language instead of what the user has selected since. The engine child is
// not affected: it is started right after SetEngineEnv, on purpose.
static std::vector<wchar_t> tool_environment_block()
{
	static const wchar_t* const kStrip[] = {
		L"POB_GAME=", L"POB_LOCALE=", L"POB_ZH_FONTFILE=", L"POB_ZH_DATADIR=", L"POB_ZH_FONT_ALL=",
		L"POB_ZH_WINDOW_OPACITY=", L"POB_ZH_BG=", L"POB_ZH_BG_BRIGHT=", L"POB_ZH_GLASS_BLUR=",
		L"POB_ZH_TREE_BG=",
	};
	std::vector<wchar_t> block;
	wchar_t* env = GetEnvironmentStringsW();
	if (!env) return block;
	for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
		bool drop = false;
		for (const wchar_t* k : kStrip) {
			if (_wcsnicmp(p, k, wcslen(k)) == 0) { drop = true; break; }
		}
		if (drop) continue;
		block.insert(block.end(), p, p + wcslen(p) + 1);
	}
	block.push_back(L'\0');
	FreeEnvironmentStringsW(env);
	return block;
}

bool SpawnToolDetached(const std::wstring& exeDir, const wchar_t* flag, InstanceKind kind,
                       unsigned long* outPid)
{
	std::wstring cmd = L"\"" + exeDir + L"pob-zh.exe\" " + flag;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	std::vector<wchar_t> env = tool_environment_block();
	if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
	                    env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT, env.empty() ? nullptr : env.data(),
	                    exeDir.c_str(), &si, &pi)) {
		// A message box tells the user; the log is what tells us WHY. GetLastError
		// is the whole answer here (5 = denied by AV/permissions, 2 = the exe moved)
		// and it is gone by the time anyone asks.
		PobLog::Error("pob", "CreateProcess for the tool window failed, GetLastError=" +
		                         std::to_string((unsigned long)GetLastError()));
		MessageBoxW(nullptr, L"無法啟動工具視窗（子程序建立失敗）。", L"PobTools",
		            MB_ICONERROR | MB_OK);
		return false;
	}
	CloseHandle(pi.hThread);
	g_instances.push_back({ pi.hProcess, std::wstring(), kind, pi.dwProcessId, nullptr });
	if (outPid) *outPid = pi.dwProcessId;
	return true;
}

int PobRunningCount()
{
	reap();
	int n = 0;
	for (const Instance& in : g_instances) if (in.kind == InstanceKind::Pob) n++;
	return n;
}

int PobRunningCountFor(const std::wstring& game)
{
	reap(); // so the answer is current
	int n = 0;
	for (const Instance& in : g_instances)
		if (in.kind == InstanceKind::Pob && in.game == game) n++;
	return n;
}

std::vector<InstanceInfo> RunningInstances()
{
	reap();
	std::vector<InstanceInfo> out;
	out.reserve(g_instances.size());
	for (Instance& in : g_instances) {
		// Re-resolve while unknown: the window appears some time after the process
		// does. Once found it is cached, but a stale handle is worse than none, so
		// verify it still exists -- and is still the GLFW window: a cached handle
		// that somehow points at anything else heals on the next call instead of
		// misdirecting messages until the next POB restart.
		if (in.hwnd && (!IsWindow(in.hwnd) || !is_glfw_window(in.hwnd))) in.hwnd = nullptr;
		if (!in.hwnd && in.pid) in.hwnd = resolve_main_window(in.pid);
		out.push_back({ in.pid, in.kind, in.game, (void*)in.hwnd });
	}
	return out;
}

bool AnyPobRunning(const std::wstring& exeDir)
{
	// PobRunningCount counts only InstanceKind::Pob, which is exactly right here:
	// an update swaps engine\*.dll, and only POB has those loaded. A tool window
	// being open must not block updates.
	if (PobRunningCount() > 0) return true;
	// A second launcher instance sees none of our handles. KeepOpen mode makes
	// "launcher already open, user double-clicks the exe again" likely, and that
	// second launcher must not swap engine\*.dll under the first one's POB.
	HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, marker_name(exeDir).c_str());
	if (!h) return false;
	CloseHandle(h);
	return true;
}

void HoldEngineRunningMarker(const std::wstring& exeDir)
{
	// Never closed on purpose: the object must live exactly as long as this
	// process, and the kernel does that for us at exit — including on a crash,
	// where an explicit release would be skipped.
	CreateMutexW(nullptr, FALSE, marker_name(exeDir).c_str());
}

void TrackHandleForTest(void* handle, const std::wstring& game)
{
	// pid 0 on purpose: RunningInstances then skips window resolution, so the
	// headless test never enumerates the real desktop.
	g_instances.push_back({ (HANDLE)handle, game, InstanceKind::Pob, 0, nullptr });
}

void TrackToolHandleForTest(void* handle, InstanceKind kind)
{
	g_instances.push_back({ (HANDLE)handle, std::wstring(), kind, 0, nullptr });
}

void TrackHandleForTestWithPid(void* handle, const std::wstring& game, unsigned long pid)
{
	// A real pid: RunningInstances will enumerate that process's windows. The
	// selftest passes its own pid and stands up the windows itself.
	g_instances.push_back({ (HANDLE)handle, game, InstanceKind::Pob, (DWORD)pid, nullptr });
}

int RunPobLaunchSelfTest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail = "") {
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};

	// Tracking works on any waitable handle, so a manual-reset event stands in
	// for a POB process: no window, no engine, milliseconds.
	g_instances.clear();
	HANDLE a = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE b = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE c = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	TrackHandleForTest(a, L"poe1");
	TrackHandleForTest(b, L"poe2");
	TrackHandleForTest(c, L"poe1");
	check("P1 three tracked instances", PobRunningCount() == 3, std::to_string(PobRunningCount()));
	check("P2 per-game counts", PobRunningCountFor(L"poe1") == 2 && PobRunningCountFor(L"poe2") == 1);

	SetEvent(b); // the MIDDLE one finishes: removal must not shift the others
	check("P3 finished instance is reaped", PobRunningCount() == 2, std::to_string(PobRunningCount()));
	check("P4 the survivors are the right ones",
	      PobRunningCountFor(L"poe1") == 2 && PobRunningCountFor(L"poe2") == 0);

	SetEvent(a);
	SetEvent(c);
	check("P5 all reaped", PobRunningCount() == 0, std::to_string(PobRunningCount()));

	// Named marker: what a SECOND launcher process uses to notice a POB it did
	// not start itself.
	check("P6 no marker means nothing running", !AnyPobRunning(exeDir));
	HANDLE m1 = CreateMutexW(nullptr, FALSE, marker_name(exeDir).c_str());
	check("P7 marker makes AnyPobRunning true", AnyPobRunning(exeDir));
	HANDLE m2 = CreateMutexW(nullptr, FALSE, marker_name(exeDir).c_str());
	CloseHandle(m1);
	check("P8 still true while a second holder remains (multiple POB windows)",
	      AnyPobRunning(exeDir));
	CloseHandle(m2);
	check("P9 false once the last holder is gone", !AnyPobRunning(exeDir));

	// A tool child must read pob-zh.ini, so the launch variables the previous
	// POB start wrote into THIS process must not reach it -- but everything else
	// (PATH, TEMP, the font dir) must.
	{
		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
		SetEnvironmentVariableW(L"POB_ZH_FONTDIR", exeDir.c_str());
		std::vector<wchar_t> blk = tool_environment_block();
		bool hasGame = false, hasLocale = false, hasFontDir = false, hasPath = false;
		for (const wchar_t* p = blk.data(); p && *p; p += wcslen(p) + 1) {
			if (_wcsnicmp(p, L"POB_GAME=", 9) == 0) hasGame = true;
			if (_wcsnicmp(p, L"POB_LOCALE=", 11) == 0) hasLocale = true;
			if (_wcsnicmp(p, L"POB_ZH_FONTDIR=", 15) == 0) hasFontDir = true;
			if (_wcsnicmp(p, L"PATH=", 5) == 0) hasPath = true;
		}
		check("E1 tool environment drops the POB launch variables", !hasGame && !hasLocale);
		check("E2 tool environment keeps everything else", hasFontDir && hasPath);
		SetEnvironmentVariableW(L"POB_GAME", nullptr);
		SetEnvironmentVariableW(L"POB_LOCALE", nullptr);
	}

	// Tools share the table with POB but must never be mistaken for it. The one
	// that matters is P12: a tool window open while an update wants to swap
	// engine\*.dll is fine, because no tool has those DLLs loaded. Getting this
	// wrong would silently stop updating for anyone who leaves the atlas planner
	// open -- with no error to show for it.
	HANDLE t1 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE t2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	TrackToolHandleForTest(t1, InstanceKind::AtlasPlanner);
	TrackToolHandleForTest(t2, InstanceKind::FilterEditor);
	check("P10 tools do not count as POB", PobRunningCount() == 0,
	      std::to_string(PobRunningCount()));
	{
		std::vector<InstanceInfo> all = RunningInstances();
		int atlas = 0, filter = 0;
		for (const InstanceInfo& i : all) {
			if (i.kind == InstanceKind::AtlasPlanner) atlas++;
			if (i.kind == InstanceKind::FilterEditor) filter++;
		}
		check("P11 both tools are listed with their kind",
		      all.size() == 2 && atlas == 1 && filter == 1,
		      "listed " + std::to_string(all.size()));
	}
	check("P12 a tool alone does not block updates", !AnyPobRunning(exeDir));

	HANDLE t3 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	TrackHandleForTest(t3, L"poe1");
	check("P13 POB alongside tools is still counted", PobRunningCount() == 1 &&
	      RunningInstances().size() == 3, std::to_string(PobRunningCount()));

	SetEvent(t1);
	SetEvent(t2);
	SetEvent(t3);
	check("P14 mixed table reaps clean", RunningInstances().empty() &&
	      PobRunningCount() == 0);

	// P15-P17: which of a child's windows is "the POB window". The engine's
	// console window is created VISIBLE and is the only visible window of a
	// fresh POB for its first ~0.3 s (the GLFW window starts hidden); with the
	// launcher polling every 0.5 s, a fallback to "any visible window" latched
	// onto the console about every other start and every appearance message
	// went there (2026-09-12). Both windows are stood up in THIS process,
	// off-screen, so the check needs no engine and no desktop interaction.
	{
		HINSTANCE hi = GetModuleHandleW(nullptr);
		const wchar_t* kConCls = L"PobToolsTest Console Class";
		const wchar_t* kGlfwCls = L"GLFW30-PobToolsTest";
		for (const wchar_t* cls : { kConCls, kGlfwCls }) {
			WNDCLASSW wc{};
			wc.lpfnWndProc = DefWindowProcW;
			wc.hInstance = hi;
			wc.lpszClassName = cls;
			RegisterClassW(&wc);
		}
		auto make = [&](const wchar_t* cls) {
			return CreateWindowExW(0, cls, L"", WS_POPUP | WS_VISIBLE, -30000, -30000, 10, 10,
			                       nullptr, nullptr, hi, nullptr);
		};
		const DWORD me = GetCurrentProcessId();
		HWND con = make(kConCls);
		check("P15 a visible non-GLFW window is not taken for the POB window",
		      con != nullptr && resolve_main_window(me) == nullptr);
		HWND glfw = make(kGlfwCls);
		check("P16 the GLFW-class window is the one resolved",
		      glfw != nullptr && resolve_main_window(me) == glfw);
		DestroyWindow(glfw);

		HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		TrackHandleForTestWithPid(ev, L"poe1", me);
		auto hwndOfMe = [&]() -> void* {
			for (const InstanceInfo& i : RunningInstances())
				if (i.pid == me) return i.hwnd;
			return (void*)(intptr_t)-1; // not listed at all
		};
		check("P17a only the console-like window: the instance has no hwnd yet",
		      hwndOfMe() == nullptr);
		glfw = make(kGlfwCls);
		check("P17b the GLFW window is picked up once it exists", hwndOfMe() == (void*)glfw);
		DestroyWindow(glfw);
		check("P17c a destroyed window is not handed out again", hwndOfMe() == nullptr);
		SetEvent(ev);
		RunningInstances(); // reaps the fake instance
		DestroyWindow(con);
		UnregisterClassW(kConCls, hi);
		UnregisterClassW(kGlfwCls, hi);
	}

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\pob_launch_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	return failures ? 2 : 0;
}

} // namespace PobLaunch
