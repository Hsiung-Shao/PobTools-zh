#include "pob_launch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace PobLaunch {
namespace {

struct Instance {
	HANDLE handle;
	std::wstring game;
};
std::vector<Instance> g_instances;

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
	MessageBoxW(nullptr, L"無法啟動 POB 子程序。", L"PobTools", MB_ICONERROR | MB_OK);
	return false;
}

} // namespace

void SetEngineEnv(const std::wstring& game, const std::wstring& locale,
                  const std::wstring& fontFile)
{
	set_env_both(L"POB_GAME", game.c_str());
	set_env_both(L"POB_LOCALE", locale.c_str());
	set_env_both(L"POB_ZH_FONTFILE", fontFile.c_str()); // r_font.cpp reads this
}

unsigned long SpawnPobAndWait(const std::wstring& launchLua)
{
	PROCESS_INFORMATION pi{};
	if (!spawn(launchLua, pi)) return (unsigned long)-1;
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return code;
}

bool SpawnPobDetached(const std::wstring& launchLua, const std::wstring& game)
{
	PROCESS_INFORMATION pi{};
	if (!spawn(launchLua, pi)) return false;
	CloseHandle(pi.hThread);
	g_instances.push_back({ pi.hProcess, game }); // handle closed when it is reaped
	return true;
}

int PobRunningCount()
{
	for (size_t i = 0; i < g_instances.size();) {
		if (WaitForSingleObject(g_instances[i].handle, 0) == WAIT_OBJECT_0) {
			CloseHandle(g_instances[i].handle);
			g_instances.erase(g_instances.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	return (int)g_instances.size();
}

int PobRunningCountFor(const std::wstring& game)
{
	PobRunningCount(); // reap first, so the answer is current
	int n = 0;
	for (const Instance& in : g_instances) if (in.game == game) n++;
	return n;
}

bool AnyPobRunning(const std::wstring& exeDir)
{
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
	g_instances.push_back({ (HANDLE)handle, game });
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
