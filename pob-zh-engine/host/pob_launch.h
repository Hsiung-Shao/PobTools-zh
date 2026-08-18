// Starting POB, and knowing whether any is still running.
//
// There are now two launch paths — host_main's blocking one (the launcher window
// is already gone) and the launcher's detached one (KeepOpen mode, window stays
// up) — and both need the same environment handed to the engine. Keeping the
// environment and the CreateProcess call in one place is the only way they stay
// in step.
//
// The engine always runs in a fresh process: SimpleGraphic.dll and its
// dependencies cannot be safely re-run in-process after RunLuaFileAsWin returns.
#pragma once

#include <string>
#include <vector>

namespace PobLaunch {

// What a tracked child process is. The launcher used to remember only POB and
// throw the tools' handles away, so it could tell you a POB was running but not
// that the atlas planner was. The window list needs both.
//
// The distinction is not cosmetic: only Pob loads engine\*.dll, and that is what
// an update must not swap out from under. Tools must NEVER count as "busy" --
// see AnyPobRunning.
enum class InstanceKind {
	Pob = 0,
	AtlasPlanner,
	FilterEditor,
	TimelessJewel,
	TranslationEditor,
};

// A child process this launcher started and that is still alive.
struct InstanceInfo {
	unsigned long pid  = 0;
	InstanceKind  kind = InstanceKind::Pob;
	std::wstring  game;        // "poe1" / "poe2"; empty for tools that are not per-game
	void*         hwnd = nullptr;  // HWND, or null while the window does not exist yet
};

// POB_GAME / POB_LOCALE / POB_ZH_FONTFILE / POB_ZH_DATADIR, set in both the
// Win32 and CRT environments so a child process inherits them. Pass dataDir empty
// for the built-in dictionaries -- the engine treats empty as "not set".
void SetEngineEnv(const std::wstring& game, const std::wstring& locale,
                  const std::wstring& fontFile, const std::wstring& dataDir,
                  bool fontApplyAll = true);

// Start POB and wait for it to close. Returns its exit code, (DWORD)-1 on
// failure to spawn.
unsigned long SpawnPobAndWait(const std::wstring& launchLua);

// Start POB and return immediately, remembering the process so PobRunningCount
// can report on it. `game` is only used to label the instance. false on failure.
//
// `outPid` receives the new process id. Callers that need it must take it from
// here rather than fishing the last entry out of RunningInstances(): that list is
// reaped and re-ordered, so "the one I just started" is an assumption, not a fact.
bool SpawnPobDetached(const std::wstring& launchLua, const std::wstring& game,
                      unsigned long* outPid = nullptr);

// Start one of the tool windows (--atlas / --filter-editor / --timeless-jewel /
// --translation-editor) and track it the same way. The child reads pob-zh.ini for
// game/locale, so callers must SaveLauncherConfig before spawning.
//
// `outPid` receives the new process id, for callers that need to find its window
// (the tabbed window mode docks tools too).
bool SpawnToolDetached(const std::wstring& exeDir, const wchar_t* flag, InstanceKind kind,
                       unsigned long* outPid = nullptr);

// How many POB processes this launcher started are still alive. Call it every
// frame: finished processes are reaped here.
//
// Counts ONLY InstanceKind::Pob. Tools are tracked in the same table but are not
// POB, and this number drives both the launcher's "N running" line and the update
// gate.
int PobRunningCount();

// Every tracked child still alive, POB and tools alike, reaped first. `hwnd` is
// resolved on demand and cached; it stays null until the child has created its
// window, so a caller must cope with null rather than assume "no window = dead".
std::vector<InstanceInfo> RunningInstances();

// How many of those were started for a given game ("poe1"/"poe2"). Two instances
// on the SAME install share POB's Settings.xml and build files, so the last one
// closed overwrites the other — worth warning about.
int PobRunningCountFor(const std::wstring& game);

// True when ANY POB is running against this install directory, including one
// started by a different launcher process. Updating swaps engine\*.dll, so a
// second launcher must not update while the first one's POB holds them.
bool AnyPobRunning(const std::wstring& exeDir);

// Called by the --engine child at startup: creates the named object AnyPobRunning
// probes for and deliberately never closes it, so the kernel releases it exactly
// when POB exits.
void HoldEngineRunningMarker(const std::wstring& exeDir);

// Test seam: track an arbitrary waitable handle as if it were a POB process.
void TrackHandleForTest(void* handle, const std::wstring& game);

// Test seam: same, for a tool instance.
void TrackToolHandleForTest(void* handle, InstanceKind kind);

// Headless checks for the tracking/reaping logic and the named marker.
int RunPobLaunchSelfTest(const std::wstring& exeDir);

} // namespace PobLaunch
