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

namespace PobLaunch {

// POB_GAME / POB_LOCALE / POB_ZH_FONTFILE, set in both the Win32 and CRT
// environments so a child process inherits them.
void SetEngineEnv(const std::wstring& game, const std::wstring& locale,
                  const std::wstring& fontFile);

// Start POB and wait for it to close. Returns its exit code, (DWORD)-1 on
// failure to spawn.
unsigned long SpawnPobAndWait(const std::wstring& launchLua);

// Start POB and return immediately, remembering the process so PobRunningCount
// can report on it. `game` is only used to label the instance. false on failure.
bool SpawnPobDetached(const std::wstring& launchLua, const std::wstring& game);

// How many POB processes this launcher started are still alive. Call it every
// frame: finished processes are reaped here.
int PobRunningCount();

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

// Headless checks for the tracking/reaping logic and the named marker.
int RunPobLaunchSelfTest(const std::wstring& exeDir);

} // namespace PobLaunch
