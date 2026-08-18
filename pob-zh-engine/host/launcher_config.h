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

// Whether POB opens as its own desktop window or as a tab inside the launcher.
//
// Tabbed keeps POB's window top-level and merely strips its frame and glues it
// to the launcher's client area -- POB itself runs completely normally and
// nothing is written to its folder. Separate is the historical behaviour and the
// default; the tabbed path is new and its every failure mode is a Win32 one, so
// it has to be chosen deliberately.
enum class WindowMode { Separate = 0, Tabbed = 1 };

// The three independently redirectable dictionary sets. Separate rather than one
// shared root: someone translating only PoE1 should not be dragged into keeping an
// external PoE2 copy in step, and the launcher's own labels are not a game at all.
enum class DictSlot { Poe1 = 0, Poe2 = 1, Launcher = 2 };
inline constexpr int kDictSlotCount = 3;

// Folder name under Data\ ("poe1" / "poe2" / "launcher").
const wchar_t* DictSlotFolder(DictSlot slot);

struct LauncherConfig {
	std::wstring   game     = L"poe1";      // "poe1" | "poe2"
	std::wstring   locale   = L"zh-rTW";    // "zh-rTW" | "en"
	LaunchExitMode exitMode = LaunchExitMode::CloseLauncher;
	StartupTab     startupTab = StartupTab::Home;
	// Separate by default. In Tabbed mode exitMode is ignored: the launcher IS
	// the window POB lives in, so "close the launcher when POB starts" has no
	// meaning.
	WindowMode     windowMode = WindowMode::Separate;
	std::wstring   fontFile;                // CJK font under Fonts\; empty = default
	// Also draw ASCII in the POB window from the selected font (engine env
	// POB_ZH_FONT_ALL). Default on; the monospaced face is always exempt.
	bool           fontApplyAll = true;
	// Per-slot dictionary folder to read instead of <exeDir>Data\<slot>\.
	// Empty = built-in. Each one points at the folder that directly CONTAINS the
	// <locale> sub-folders, i.e. exactly what you get by copying Data\poe1
	// somewhere. Lets a translator keep a working copy OUTSIDE the install and
	// still see it rendered by the real POB -- and, because updates only ever
	// write to the install directory, that copy cannot be overwritten.
	std::wstring   dataDir[kDictSlotCount];
	// Whether an update may replace the translation dictionaries. Off is for
	// people editing translations themselves; see IsTranslationDataRel.
	bool           updateTranslations = true;
};

// ---- external dictionary folders -------------------------------------------

enum class DataDirStatus {
	Builtin,         // nothing configured
	External,        // configured and usable
	Missing,         // the configured folder does not exist
	WrongShape,      // holds meta.json directly -- this IS a <locale> folder, go up one
	TooShallow,      // holds <slot>\<locale>\meta.json -- go down into <slot>
	NoDictionaries,  // exists but holds no <locale>\meta.json at all
};

struct DictDirInfo {
	DataDirStatus status = DataDirStatus::Builtin;
	std::wstring  root;                 // folder to use, always valid, trailing '\'
	std::wstring  configured;           // what the user set, verbatim
	bool          insideInstall = false;   // under exeDir => updates will overwrite it
	// "zh-rTW" -> number of *.json files, for the settings page
	std::vector<std::pair<std::string, int>> found;
	// Files the built-in meta.json loads that the external copy's meta.json does
	// not list. The engine loads only what load_order names, so a dictionary added
	// by a later release is SILENTLY absent from an older external copy.
	std::vector<std::string> staleLoadOrder;
	// Suggested correction for WrongShape / TooShallow, ready to put in the field.
	std::wstring  suggestion;
};

// Decide which folder to read one dictionary set from. Never fails: anything
// wrong with the configured path falls back to <exeDir>Data\<slot>\ and says so
// in `status`, because a bad setting must not leave POB with no translations.
DictDirInfo ResolveDictDir(const std::wstring& exeDir, DictSlot slot, const std::wstring& configured);

// <exeDir>Data\<slot>\ -- the shipped folder for a slot, with trailing backslash.
std::wstring BuiltinDictDir(const std::wstring& exeDir, DictSlot slot);

// ---- installed interface languages ------------------------------------------

// One language offered in the launcher's language picker.
struct LocaleInfo {
	std::string  id;                  // folder name, e.g. "zh-rTW"; "en" for the original text
	std::string  displayName;         // meta.json's display_name, or id when absent
	bool         slot[kDictSlotCount] = { false, false, false }; // which sets have it
};

// Languages actually present on disk, taken from the folders under each slot's
// resolved root -- so adding Data\poe1\ja-JP\ is all it takes to offer Japanese.
//
// Two rules that are not obvious:
//  - "en" is ALWAYS first and needs no folder. English is not a translation; it
//    is what the engine shows when it finds no dictionary, so it cannot be
//    discovered by scanning.
//  - The result is the UNION across the three sets, not the intersection. A
//    language present only for PoE1 is still selectable; PoE2 then behaves
//    exactly as it does for "en" (passes the original text through), which is a
//    rule the user already understands rather than a second special case.
std::vector<LocaleInfo> ListInstalledLocales(const std::wstring& exeDir, const LauncherConfig& cfg);

// Index into `locales` for `want`, falling back to zh-rTW and then to en (index
// 0). Never returns out of range: a configured language whose folder was deleted
// must land somewhere sensible instead of leaving the picker blank.
int PickLocaleIndex(const std::vector<LocaleInfo>& locales, const std::wstring& want);

// Is `path` the same folder as `parentDir` or inside it? Case-insensitive, and
// "D:\app\Data2" is NOT inside "D:\app\Data". Exposed for the selftest -- the
// prefix traps here are exactly the kind that pass a casual eyeball.
bool PathIsInside(const std::wstring& parentDir, const std::wstring& path);

// Copy one slot's built-in dictionaries into `dest`, producing the exact layout
// ResolveDictDir expects. This is what makes the feature usable by someone who is
// not going to be told which folder level to point at.
// Returns the number of files copied, or -1 on failure (*err gets a UTF-8 reason).
int CopyBuiltinDictionary(const std::wstring& exeDir, DictSlot slot,
                          const std::wstring& dest, std::string* err);

// Does `dest` already hold a dictionary (any <locale>\meta.json)? The caller
// confirms before overwriting -- a translator's work is in there.
bool DictionariesPresentAt(const std::wstring& dest);

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
