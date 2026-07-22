// pob-zh host: launcher UI + thin loader for the SimpleGraphic engine.
//
// Process flow:
//   pob-zh.exe                      -> dark ImGui launcher (language / POE1-POE2 /
//                                      return-to-launcher), then spawns itself with
//                                      --engine and waits; loops if configured.
//   pob-zh.exe --engine <Launch.lua>-> internal: load SimpleGraphic.dll and run POB
//                                      (POB_GAME/POB_LOCALE inherited from parent).
//   pob-zh.exe <path>               -> legacy CLI: skip the UI, run POB directly in
//                                      this process (path = Launch.lua or POB folder).
//
// The engine always runs in a fresh process: SimpleGraphic.dll and its deps
// (LuaJIT, curl, GLFW/ANGLE globals, POB worker threads) cannot be safely
// re-run after RunLuaFileAsWin returns, so "return to launcher" re-spawns.
//
// The external POB folder is never modified, so it can keep self-updating.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <cwchar>
#include <string>
#include <vector>

#include "launcher_config.h"
#include "launcher_ui.h"
#include "launcher_editor.h"
#include "filter_editor.h"
#include "atlas_planner.h"
#include "atlas_tree_data.h"
#include "atlas_import.h"
#include "atlas_stat_agg.h"
#include "atlas_update.h"
#include "atlas_diff.h"
#include "atlas_version_index.h"
#include "filter_selftest.h"
#include "timeless_jewel.h"
#include "timeless_jewel_ui.h"
#include "passive_tree_data.h"
#include "ui_theme.h"

#pragma comment(lib, "shell32.lib")

typedef int (*RunLuaFileAsWin_t)(int argc, char** argv);

// Set an environment variable in BOTH the Win32 and CRT environments.
// This exe uses the STATIC CRT, so the engine's getenv() reads a different
// (UCRT) environment: ucrtbase.dll initialises when the first /MD DLL loads
// and snapshots the Win32 environment AT THAT MOMENT. SetEnvironmentVariableW
// is therefore the critical half here — it runs before any DLL is loaded.
// Do not remove either call.
static void set_env_both(const wchar_t* var, const wchar_t* val)
{
	SetEnvironmentVariableW(var, val);
	_wputenv_s(var, val);
}

// Convert a wide string to UTF-8.
static std::string to_utf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], needed, nullptr, nullptr);
	return s;
}

// Convert a UTF-8 string to wide.
static std::wstring from_utf8(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], needed);
	return w;
}

// Full path of this exe.
static std::wstring exe_path()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return std::wstring(buf);
}

// Directory of this exe, with trailing backslash.
static std::wstring exe_dir()
{
	std::wstring p = exe_path();
	size_t slash = p.find_last_of(L'\\');
	return (slash == std::wstring::npos) ? p : p.substr(0, slash + 1);
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Accept a Launch.lua file or a POB folder; return the Launch.lua path or empty.
static std::wstring launch_lua_from(std::wstring p)
{
	if (p.empty()) return L"";
	if (file_exists(p)) return p;
	if (dir_exists(p)) {
		if (p.back() != L'\\') p += L'\\';
		std::wstring lua = p + L"Launch.lua";
		if (file_exists(lua)) return lua;
	}
	return L"";
}

// Legacy resolution for the CLI path mode: POB_PATH env, then sibling POE1 folder.
static std::wstring resolve_launch_lua_legacy(const std::wstring& dir)
{
	wchar_t env[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"POB_PATH", env, MAX_PATH);
	if (n > 0 && n < MAX_PATH) {
		std::wstring r = launch_lua_from(std::wstring(env, n));
		if (!r.empty()) return r;
	}
	std::wstring sibling = dir + L"PathOfBuildingCommunity\\Launch.lua";
	if (file_exists(sibling)) return sibling;
	return L"";
}

// Set POB_GAME / POB_LOCALE for the engine, unless already set in the environment.
// Falls back to pob-zh.ini ([PobTools] Game / Locale, legacy [PoeCharm]) then to poe1 / zh-rTW.
// Used by the CLI path mode; in launcher mode the UI choices are set explicitly,
// and the --engine child inherits them from the parent.
static void apply_locale_env(const std::wstring& dir)
{
	std::wstring ini = dir + L"pob-zh.ini";

	auto ensure = [&](const wchar_t* var, const wchar_t* iniKey, const wchar_t* fallback) {
		if (GetEnvironmentVariableW(var, nullptr, 0) > 0) return; // already set, respect it
		wchar_t val[128];
		// New [PobTools] section, falling back to legacy [PoeCharm] for old ini files.
		static const wchar_t* kSentinel = L"\x01\x7f";
		GetPrivateProfileStringW(L"PobTools", iniKey, kSentinel, val, 128, ini.c_str());
		if (wcscmp(val, kSentinel) == 0)
			GetPrivateProfileStringW(L"PoeCharm", iniKey, fallback, val, 128, ini.c_str());
		set_env_both(var, val);
	};

	ensure(L"POB_GAME", L"Game", L"poe1");
	ensure(L"POB_LOCALE", L"Locale", L"zh-rTW");
}

// Load SimpleGraphic.dll (from the engine DLL directory) and run POB.
// Blocks until POB exits.
static int run_engine(const std::wstring& dllDir, const std::wstring& launchLua)
{
	std::wstring dllPath = dllDir + L"SimpleGraphic.dll";
	HMODULE engine = LoadLibraryW(dllPath.c_str());
	if (!engine) {
		MessageBoxW(nullptr, L"無法載入 SimpleGraphic.dll。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}

	RunLuaFileAsWin_t RunLuaFileAsWin = (RunLuaFileAsWin_t)GetProcAddress(engine, "RunLuaFileAsWin");
	if (!RunLuaFileAsWin) {
		MessageBoxW(nullptr, L"SimpleGraphic.dll 缺少 RunLuaFileAsWin 匯出。", L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}

	// argv[0] = Launch.lua path (engine sets the working dir to its parent folder).
	std::string luaUtf8 = to_utf8(launchLua);
	std::vector<char> arg0(luaUtf8.begin(), luaUtf8.end());
	arg0.push_back('\0');
	char* argv[1] = { arg0.data() };

	return RunLuaFileAsWin(1, argv);
}

// Relaunch marker, written by the engine right before it spawns POB's
// runtime updater (Update.exe): the updater finishes by start-ing this exe,
// and the marker tells us to skip the launcher UI once and reopen that POB.
static std::wstring relaunch_marker_path(const std::wstring& dir)
{
	return dir + L"pob-zh.relaunch";
}

// Read and consume the marker; returns the Launch.lua path or empty.
static std::wstring take_relaunch_marker(const std::wstring& dir)
{
	std::wstring marker = relaunch_marker_path(dir);
	HANDLE h = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return L"";
	char buf[2048];
	DWORD read = 0;
	ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
	CloseHandle(h);
	DeleteFileW(marker.c_str());
	buf[read] = '\0';
	return from_utf8(std::string(buf, read));
}

// Spawn ourselves with --engine and wait for POB to exit.
// POB_GAME / POB_LOCALE / POB_ZH_FONTDIR are inherited through the environment.
static DWORD spawn_engine_and_wait(const std::wstring& launchLua)
{
	std::wstring exe = exe_path();
	std::wstring cmd = L"\"" + exe + L"\" --engine \"" + launchLua + L"\"";
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		MessageBoxW(nullptr, L"無法啟動 POB 子程序。", L"PobTools", MB_ICONERROR | MB_OK);
		return (DWORD)-1;
	}
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return code;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	std::wstring dir = exe_dir();

	// All DLLs (SimpleGraphic.dll + its deps + our delay-loaded glfw3 and
	// libGLESv2) live in <exe dir>\engine; fall back to the exe dir itself
	// for the legacy flat layout. SetDllDirectoryW must happen before the
	// first glfw/GL call so the delay-loads resolve from here, and keeps the
	// POB folder's own bundled DLLs out of the search path.
	std::wstring engineDir = dir + L"engine\\";
	if (!file_exists(engineDir + L"SimpleGraphic.dll")) {
		engineDir = dir;
	}
	SetDllDirectoryW(engineDir.c_str());

	// Tell the engine where to find our bundled CJK fonts (dir\Fonts\FZ_ZY.ttf),
	// so the external POB folder stays pristine.
	set_env_both(L"POB_ZH_FONTDIR", dir.c_str());

	int argc = 0;
	LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring arg1 = (argvW && argc >= 2) ? argvW[1] : L"";
	std::wstring arg2 = (argvW && argc >= 3) ? argvW[2] : L"";
	std::wstring arg3 = (argvW && argc >= 4) ? argvW[3] : L"";
	std::wstring arg4 = (argvW && argc >= 5) ? argvW[4] : L"";
	if (argvW) LocalFree(argvW);

	// Headless timeless-jewel engine checks.
	if (arg1 == L"--tj-selftest") {
		return RunTimelessJewelSelfTest(dir);
	}
	if (arg1 == L"--tj") { // --tj <jewelType> <seed> <nodeId>
		return RunTimelessJewelCli(dir, _wtoi(arg2.c_str()), _wtoi(arg3.c_str()), _wtoi(arg4.c_str()));
	}

	// Headless passive-tree data check (node/socket/radius counts; report file).
	if (arg1 == L"--passive-tree-selftest") {
		return RunPassiveTreeSelfTest(dir);
	}
	// Headless one-frame canvas render to pt_render.bmp (debug aid).
	if (arg1 == L"--pt-render") { // --pt-render [zoom cx cy]
		return RunPassiveTreeRender(dir, (float)_wtof(arg2.c_str()),
		                            (float)_wtof(arg3.c_str()), (float)_wtof(arg4.c_str()));
	}

	// Headless atlas-planner logic check (no window; report file + exit code).
	if (arg1 == L"--atlas-selftest") {
		return RunAtlasSelfTest(dir);
	}

	// Headless stat-aggregation check (synthetic cases; console report).
	if (arg1 == L"--atlas-agg-selftest") {
		return RunAtlasAggSelfTest(dir);
	}

	// Headless cross-season diff logic check (synthetic old/new trees).
	if (arg1 == L"--atlas-diff-selftest") {
		return RunAtlasDiffSelfTest(dir);
	}

	// Headless version-registry logic check (semver / prune / rolling retention).
	if (arg1 == L"--atlas-index-selftest") {
		return RunAtlasVersionIndexSelfTest(dir);
	}

	// Headless cross-season diff report: --atlas-diff <oldVer> <newVer>.
	if (arg1 == L"--atlas-diff") {
		return RunAtlasDiffCli(arg2, arg3, dir);
	}

	// Headless filter-editor data-layer check (synthetic cases; console report).
	if (arg1 == L"--filter-selftest") {
		return RunFilterSelfTest(dir);
	}

	// Headless shared-theme invariants (no GLFW window or renderer required).
	if (arg1 == L"--ui-theme-selftest") {
		return PobUi::RunThemeSelfTest() ? 0 : 1;
	}

	// Headless new-season atlas data import: --atlas-import <path to data.json>.
	if (arg1 == L"--atlas-import") {
		return RunAtlasImportCli(arg2, dir);
	}

	// Headless auto update: check GitHub tags, download, import, zh mapping.
	if (arg1 == L"--atlas-update") {
		return RunAtlasUpdateCli(dir);
	}

	// Offline zh-mapping rebuild: --atlas-zh <data.json> <TC Atlas.json>.
	if (arg1 == L"--atlas-zh") {
		return RunAtlasZhCli(arg2, arg3, dir);
	}

	// Open the atlas planner directly (shortcut-friendly).
	if (arg1 == L"--atlas") {
		ShowAtlasPlanner(dir, LoadLauncherConfig(dir + L"pob-zh.ini").locale);
		return 0;
	}

	// Internal engine child: env already inherited from the launcher parent.
	if (arg1 == L"--engine") {
		std::wstring launchLua = launch_lua_from(arg2);
		if (launchLua.empty()) return 1;
		apply_locale_env(dir); // no-op when inherited; safety net for manual use
		return run_engine(engineDir, launchLua);
	}

	// Legacy CLI: explicit path = skip the UI, run in-process (old behaviour).
	if (!arg1.empty()) {
		std::wstring launchLua = launch_lua_from(arg1);
		if (launchLua.empty()) launchLua = resolve_launch_lua_legacy(dir);
		if (launchLua.empty()) {
			MessageBoxW(nullptr,
				L"找不到 Path of Building 的 Launch.lua。\n\n"
				L"請將 PathOfBuildingCommunity 資料夾放在 pob-zh.exe 旁邊,\n"
				L"或設定環境變數 POB_PATH 指向 POB 安裝目錄。",
				L"PobTools", MB_ICONERROR | MB_OK);
			return 1;
		}
		apply_locale_env(dir);
		return run_engine(engineDir, launchLua);
	}

	// Pre-flight: glfw3.dll / libGLESv2.dll are delay-loaded — if they are
	// missing, the first glfw call dies with an unhelpful SEH exception
	// (0xC06D007E) instead of a loader error, so check up front.
	if (!file_exists(engineDir + L"glfw3.dll") || !file_exists(engineDir + L"libGLESv2.dll")) {
		MessageBoxW(nullptr,
			L"engine 資料夾不完整（缺少 glfw3.dll 或 libGLESv2.dll）。\n"
			L"請重新解壓縮完整的 pob-zh 套件。",
			L"PobTools", MB_ICONERROR | MB_OK);
		return 1;
	}

	// Launcher mode. A pending relaunch marker (left by the engine when POB
	// self-updated) bypasses the UI once and reopens the updated POB directly.
	std::wstring ini = dir + L"pob-zh.ini";
	std::wstring pendingLua = take_relaunch_marker(dir);
	for (;;) {
		LauncherConfig cfg = LoadLauncherConfig(ini);
		InstallInfo installs = DetectInstalls(dir);

		std::wstring launchLua;
		if (!pendingLua.empty()) {
			launchLua = launch_lua_from(pendingLua);
			pendingLua.clear();
		}
		if (launchLua.empty()) {
			LauncherResult res = ShowLauncher(cfg, installs, dir);
			SaveLauncherConfig(ini, cfg); // remember choices regardless of outcome
			if (res == LauncherResult::OpenEditor) {
				ShowEditor(dir, cfg.game, cfg.locale);
				continue; // back to the launcher screen
			}
			if (res == LauncherResult::OpenFilterEditor) {
				ShowFilterEditor(dir, cfg.game, cfg.locale);
				continue; // back to the launcher screen
			}
			if (res == LauncherResult::OpenAtlasPlanner) {
				ShowAtlasPlanner(dir, cfg.locale);
				continue; // back to the launcher screen
			}
			if (res == LauncherResult::OpenTimelessJewel) {
				ShowTimelessJewel(dir, cfg.locale);
				continue; // back to the launcher screen
			}
			if (res != LauncherResult::Launch) {
				return 0;
			}
			launchLua = (cfg.game == L"poe2") ? installs.poe2Lua : installs.poe1Lua;
			// POB_PATH still works as an override source when the sibling folder is absent.
			if (launchLua.empty()) launchLua = resolve_launch_lua_legacy(dir);
			if (launchLua.empty()) continue; // UI should have prevented this; just re-show
		}

		set_env_both(L"POB_GAME", cfg.game.c_str());
		set_env_both(L"POB_LOCALE", cfg.locale.c_str());
		set_env_both(L"POB_ZH_FONTFILE", cfg.fontFile.c_str()); // r_font.cpp reads this

		spawn_engine_and_wait(launchLua);

		// POB self-updated: its updater is about to start a fresh pob-zh.exe
		// (which will consume the marker), so this instance bows out instead
		// of racing it with a launcher window.
		if (GetFileAttributesW(relaunch_marker_path(dir).c_str()) != INVALID_FILE_ATTRIBUTES) return 0;

		if (!cfg.returnToLauncher) return 0;
	}
}
