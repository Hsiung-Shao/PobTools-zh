// Launcher settings (pob-zh.ini) and sibling POB install detection.
#pragma once

#include <string>
#include <vector>

struct LauncherConfig {
	std::wstring game;        // "poe1" | "poe2"
	std::wstring locale;      // "zh-rTW" | "en"
	bool returnToLauncher;    // show the launcher again after POB exits
	std::wstring fontFile;    // CJK font filename under Fonts\ (e.g. "NotoSansTC-Regular.ttf")
};

// Detected sibling POB installs; empty string = not found.
struct InstallInfo {
	std::wstring poe1Lua;     // <exe dir>\PathOfBuildingCommunity\Launch.lua
	std::wstring poe2Lua;     // <exe dir>\PathOfBuildingCommunity-PoE2-Portable\Launch.lua
	std::string poe1Version;  // manifest.xml Version number ("2.65.0"); "" when unknown
	std::string poe2Version;
};

LauncherConfig LoadLauncherConfig(const std::wstring& iniPath);
void SaveLauncherConfig(const std::wstring& iniPath, const LauncherConfig& cfg);
InstallInfo DetectInstalls(const std::wstring& exeDir);

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
