// Launcher settings (pob-zh.ini) and sibling POB install detection.
#pragma once

#include <string>
#include <vector>

// What the launcher does once POB starts. One enum rather than two booleans:
// "return afterwards" and "stay open" are mutually exclusive, and two bools have
// an illegal both-set state that a hand-edited ini can produce and that every
// read/toggle/write site would have to defend against separately. Clamped once
// in LoadLauncherConfig, and the UI falls out as a single radio group.
enum class LaunchExitMode {
	CloseLauncher   = 0,  // launcher exits with POB (the historical default)
	ReturnAfterExit = 1,  // launcher reappears when POB closes
	KeepOpen        = 2,  // launcher stays up; POB can be started more than once
};

// Which tab the launcher opens on. Settings is deliberately not offered: landing
// on a settings page is never what someone wants on startup.
enum class StartupTab { Home = 0, Versions = 1 };

struct LauncherConfig {
	std::wstring   game     = L"poe1";      // "poe1" | "poe2"
	std::wstring   locale   = L"zh-rTW";    // "zh-rTW" | "en"
	LaunchExitMode exitMode = LaunchExitMode::CloseLauncher;
	StartupTab     startupTab = StartupTab::Home;
	std::wstring   fontFile;                // CJK font under Fonts\; empty = default
};

// Detected POB installs; empty string = not found. Detection order per slot:
// canonical folder names first (PathOfBuildingCommunity[-Portable] / -PoE2-Portable),
// then any first-level subfolder holding Launch.lua (name containing "poe2" =>
// the PoE2 fork, sorted for a deterministic pick), then the exe directory itself
// (pob-zh.exe dropped inside a POB folder), classified by its own folder name.
struct InstallInfo {
	std::wstring poe1Dir;     // install folder, no trailing backslash
	std::wstring poe2Dir;
	std::wstring poe1Lua;     // <poe1Dir>\Launch.lua
	std::wstring poe2Lua;     // <poe2Dir>\Launch.lua
	std::string poe1Version;  // manifest.xml Version number ("2.65.0"); "" when unknown
	std::string poe2Version;
};

LauncherConfig LoadLauncherConfig(const std::wstring& iniPath);
void SaveLauncherConfig(const std::wstring& iniPath, const LauncherConfig& cfg);
InstallInfo DetectInstalls(const std::wstring& exeDir);

// Detected PoE1 POB install dir with a trailing backslash; L"" when none.
// For components that read files out of the PoE1 install (TreeData, jewels).
std::wstring FindPoe1Dir(const std::wstring& exeDir);

// Headless check of the DetectInstalls rules against synthetic folder layouts
// under %TEMP%; report at <exeDir>PobTools\detect_selftest.txt, 0 = all pass.
int RunDetectInstallsSelfTest(const std::wstring& exeDir);

// Headless check of the ini parsing rules -- migration from the old
// ReturnToLauncher boolean, clamping of out-of-range values, and round-tripping.
// Same report convention as above; 0 = all pass.
int RunLauncherConfigSelfTest(const std::wstring& exeDir);

// Default bundled CJK font (open-source Noto Sans TC). FZ_ZY.ttf stays available
// as a selectable fallback.
extern const wchar_t* const kDefaultFontFile;

// Filenames of every *.ttf under <exe dir>\Fonts (for the font picker).
std::vector<std::wstring> ListAvailableFonts(const std::wstring& exeDir);

// Full path to the font to load: the requested fontFile if present, otherwise
// falls back to the Noto default, then FZ_ZY, then any Fonts\*.ttf.
std::wstring ResolveFontPath(const std::wstring& exeDir, const std::wstring& fontFile);

// Convenience for the editor/atlas entry points: read the configured font from
// <exeDir>\pob-zh.ini and resolve it to a full path (with the same fallbacks).
std::wstring ResolveConfiguredFontPath(const std::wstring& exeDir);
