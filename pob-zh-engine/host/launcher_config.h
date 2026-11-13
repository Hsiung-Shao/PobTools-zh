// Launcher settings (pob-zh.ini) and sibling POB install detection.
#pragma once

#include <string>
#include <vector>

// What the launcher does once POB starts. An enum, not two bools (no illegal both-set state
// from a hand-edited ini); clamped once in LoadLauncherConfig.
enum class LaunchExitMode {
	CloseLauncher   = 0,  // launcher exits with POB (the historical default)
	ReturnAfterExit = 1,  // launcher reappears when POB closes
	KeepOpen        = 2,  // launcher stays up; POB can be started more than once
};

// Which tab the launcher opens on (Settings deliberately not offered).
enum class StartupTab { Home = 0, Versions = 1 };

// Whether POB opens as its own desktop window or as a tab inside the launcher.
//
// Tabbed keeps POB's window top-level, strips its frame and glues it to the launcher's client
// area; nothing is written to POB's folder. Separate is the default (tabbed failure modes are Win32 ones).
enum class WindowMode { Separate = 0, Tabbed = 1 };

// The three independently redirectable dictionary sets (separate roots: translating PoE1 alone
// must not require an external PoE2 copy).
enum class DictSlot { Poe1 = 0, Poe2 = 1, Launcher = 2 };
inline constexpr int kDictSlotCount = 3;

// Folder name under Data\ ("poe1" / "poe2" / "launcher").
const wchar_t* DictSlotFolder(DictSlot slot);

// Dictionary files the settings page lets the user switch off. The engine still
// follows meta.json's load_order; it only skips a file switched off here (env
// POB_ZH_DICT_SWITCHES, see DictSwitchesEnv). To make another file switchable,
// add a row here plus its label/tip in launcher_ui.cpp. On by default.
struct OptionalDict {
	const char*    file;     // as listed in meta.json load_order
	const wchar_t* iniKey;   // [PobTools] key, "1" / "0"
};
inline constexpr OptionalDict kOptionalDicts[] = {
	{ "ui_tooltips.json", L"DictUiTooltips" },
};
inline constexpr int kOptionalDictCount = (int)(sizeof(kOptionalDicts) / sizeof(kOptionalDicts[0]));

// launcher-only zoom and window size
//
// "Font size" is the ONE user-facing knob for the launcher's scale: the body font size becomes a
// zoom factor folded into the <logical> * scale every dimension already uses. Applies to the
// launcher and standalone tool windows only, never to POB.
inline constexpr int kLauncherFontSizeDefault = 19;   // launcher_ui.cpp kFontSize
inline constexpr int kLauncherFontSizeMin     = 14;
inline constexpr int kLauncherFontSizeMax     = 26;

// Out of range (hand-edited, non-numeric -> 0, a future build's value) means the default, never
// "as small as it goes": 0 is what GetPrivateProfileIntW returns for text.
// The new interface's zoom (percent) and base font size (px) follow the same rule.
inline constexpr int kModernZoomDefault = 100, kModernZoomMin = 70, kModernZoomMax = 200;
inline constexpr int kModernFontSizeDefault = 13, kModernFontSizeMin = 11, kModernFontSizeMax = 18;
inline int ClampModernZoom(int v) { return (v < kModernZoomMin || v > kModernZoomMax) ? kModernZoomDefault : v; }
inline int ClampModernFontSize(int v) { return (v < kModernFontSizeMin || v > kModernFontSizeMax) ? kModernFontSizeDefault : v; }

// The new interface's look (settings page, "Appearance"); unknown values read as the default.
//   theme  -- one of the page's built-in themes; anything else is "slate"
//   accent -- "#rrggbb" (lower-cased) or empty = the theme's own accent
//   font   -- a file name under Fonts\ or empty = follow the launcher's Font=.
//             Only a bare *.ttf name is kept: the page loads it from the
//             fonts.pobtools mapping, which must never be handed a path.
std::wstring NormalizeModernTheme(const std::wstring& v);
std::wstring NormalizeModernAccent(const std::wstring& v);
std::wstring NormalizeModernFont(const std::wstring& v);

inline int ClampLauncherFontSize(int v)
{
	return (v < kLauncherFontSizeMin || v > kLauncherFontSizeMax) ? kLauncherFontSizeDefault : v;
}
inline float LauncherZoom(int fontSize)
{
	return (float)ClampLauncherFontSize(fontSize) / (float)kLauncherFontSizeDefault;
}

// POB panel opacity in percent (Windows only; see PobLaunch::SetEngineEnv and ApplyPobWindowOpacity).
// 100 = opaque = feature off (also what a garbage ini value means); 0 = only text and controls
// remain over the passive tree. The window itself never becomes see-through.
inline constexpr int kWindowOpacityMin     = 0;
inline constexpr int kWindowOpacityDefault = 100;
inline int ClampWindowOpacity(int v)
{
	if (v < 0 || v > 100) return kWindowOpacityDefault;
	return v;
}
// Background image brightness (percent) and liquid-glass blur (percent).
inline constexpr int kBgBrightDefault = 50;
inline int ClampPercent(int v, int fallback)
{
	return (v < 0 || v > 100) ? fallback : v;
}

// Everything the "Appearance" tab controls, kept per game.
struct AppearanceConfig {
	// Opacity of the chrome panels (side bar, top bar, tree toolbar) in percent.
	int            windowOpacity = kWindowOpacityDefault;
	// Background image: file name under PobTools\Backgrounds\ (empty = POB's
	// own striped backdrop) and its brightness in percent.
	std::wstring   background;
	int            bgBright = kBgBrightDefault;
	// Liquid-glass blur strength of the chrome panels (0 = plain fill).
	int            glassBlur = 0;
	// Opacity of the passive tree's own tiled backdrop (the layer under the
	// nodes), percent. 100 = as upstream; lower lets the background image show
	// through the tree area too.
	int            treeBg = 100;
};
// The new interface's own copy of the same five knobs, per game. `follow` (default) = use the
// launcher's set; the page clears it on the first change.
struct ModernLookConfig {
	bool            follow = true;
	AppearanceConfig look;
	// Where the picture shows: 0 behind the whole window, 1 only behind the
	// passive tree (every other tab stays solid). The launcher has no such knob,
	// so following it means 0.
	int             bgScope = 0;
};
// A background the page may load: a bare png/jpg/jpeg/webp file name under
// PobTools\Backgrounds\, never a path (it is served over bg.pobtools). With
// allowVideo also mp4/webm -- the new interface can play those, the launcher
// and the classic POB window (ImGui / OpenGL, no video decoder) cannot.
std::wstring NormalizeBackgroundFile(const std::wstring& v, bool allowVideo = false);
// mp4 / webm: the page draws it with <video> instead of a CSS image.
bool IsVideoBackground(const std::wstring& v);

// "poe1" -> 0, "poe2" -> 1 (anything else counts as PoE1).
inline int GameIndex(const std::wstring& game) { return game == L"poe2" ? 1 : 0; }
inline const wchar_t* GameKeySuffix(int idx) { return idx == 1 ? L"Poe2" : L"Poe1"; }

// Window dimensions are stored in PHYSICAL pixels, as GLFW reports them. 0 = the mode's default;
// anything implausible is treated as 0 rather than clamped to the nearest edge.
inline constexpr int kWindowDimMin = 400;
inline constexpr int kWindowDimMax = 16384;
inline int ClampWindowDim(int v)
{
	return (v < kWindowDimMin || v > kWindowDimMax) ? 0 : v;
}

struct LauncherConfig {
	std::wstring   game     = L"poe1";      // "poe1" | "poe2"
	std::wstring   locale   = L"zh-rTW";    // "zh-rTW" | "en"
	LaunchExitMode exitMode = LaunchExitMode::CloseLauncher;
	StartupTab     startupTab = StartupTab::Home;
	// In Tabbed mode exitMode is ignored: the launcher IS the window POB lives in.
	WindowMode     windowMode = WindowMode::Separate;
	// Remembered launcher window size, physical pixels, 0 = the mode's default.
	// One pair per window mode so switching modes doesn't drag the other's size along.
	int            winW = 0, winH = 0;          // WindowMode::Separate
	int            tabWinW = 0, tabWinH = 0;    // WindowMode::Tabbed
	// The new (WebView2) interface window, --modern-ui; its own pair, 0 = default.
	int            modernWinW = 0, modernWinH = 0;
	// The new interface's own scale knobs: WebView2 zoom (percent) and base font size (px).
	int            modernZoom = kModernZoomDefault;
	int            modernFontSize = kModernFontSizeDefault;
	std::wstring   modernTheme = L"slate";
	std::wstring   modernAccent;            // "#rrggbb"; empty = the theme's own
	std::wstring   modernFont;              // Fonts\ file name; empty = the launcher's
	// 0 = classic POB window, 1 = new interface theme 1 (ui\index.html),
	// 2 = new interface theme 2 (ui\theme2.html): what the launcher's
	// "Launch" button opens, for either game (settings page, "Default
	// interface"). A machine without WebView2 and a POB the bridge gate refused
	// open the classic window regardless (ModernUiUsableFor).
	int            uiMode = 0;
	std::wstring   fontFile;                // CJK font under Fonts\; empty = default
	// Also draw ASCII in the POB window from the selected font (engine env
	// POB_ZH_FONT_ALL). Default on; the monospaced face is always exempt.
	bool           fontApplyAll = true;
	// Parallel to kOptionalDicts: 1 = the engine loads that file (default).
	std::vector<char> optionalDictOn = std::vector<char>(kOptionalDictCount, 1);
	// Launcher body font size in px at 100% DPI; drives the whole-UI zoom (see
	// LauncherZoom above). POB is unaffected.
	int            fontSize = kLauncherFontSizeDefault;
	// POB window appearance, one set per game (PoE1 / PoE2 have different
	// trees and different tastes); index with GameIndex(). Windows only; the
	// launcher's "Appearance" tab edits them and pushes changes live to the POB
	// windows of that game; SetEngineEnv passes the starting game's set.
	AppearanceConfig look[2];
	// The new interface's background set, per game (settings page "Background").
	ModernLookConfig modernLook[2];
	// Per-slot dictionary folder to read instead of <exeDir>Data\<slot>\ (empty = built-in).
	// Points at the folder that directly CONTAINS the <locale> sub-folders (a copy of Data\poe1);
	// updates only write to the install directory, so that copy cannot be overwritten.
	std::wstring   dataDir[kDictSlotCount];
	// Whether an update may replace the translation dictionaries. Off is for
	// people editing translations themselves; see IsTranslationDataRel.
	bool           updateTranslations = true;
	// Install a new PobTools release by itself at startup. OFF by default and must stay so (applying
	// closes and reopens the program). See ShouldAutoApplyApp for the narrow moment it may act.
	bool           autoApplyAppUpdate = false;
	// Take the beta line: app updates come from the newest release of any kind (prereleases included).
	// Turning it off never downgrades (PlanUpdates only offers a HIGHER version).
	// ⚠ The translation-data line is NOT forked: both channels share data-<n>.
	bool           betaChannel = false;
	// HTTP proxy for the launcher's own downloads, "host:port". Empty = follow the system proxy.
	// Applied via HttpSetManualProxy (http_client.h).
	std::wstring   proxy;
	// The hang/crash watchdog (hang_watch.h), passed to POB as POB_ZH_HANGWATCH. On by default and
	// deliberately not in the UI; the ini key lets a machine where suspending threads is a problem opt out.
	bool           hangWatch = true;
	// POB window frame-rate caps (host/pob_frame_cap.h), fps, 0 = no cap. Global,
	// not per game. Passed as POB_ZH_FPS_FG / POB_ZH_FPS_BG and pushed live.
	int            pobFpsForeground = 60;
	int            pobFpsBackground = 15;
	// Opt-in performance log for POB (host/perf_log.h), POB_ZH_PERFLOG. Off by
	// default; the settings page turns it on when someone is chasing a report.
	bool           perfLog = false;
	// "Open in PoB" links (pob:// / pob2://, host/pob_protocol.h) handled by this
	// install. Off by default: turning it on writes HKCU\Software\Classes and
	// takes the links over from an official POB install.
	bool           pobProtocol = false;
};

// uiMode -> the page file under ui\ that the new interface opens.
const wchar_t* ModernUiPage(int uiMode);

// external dictionary folders

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

// installed interface languages

// One language offered in the launcher's language picker.
struct LocaleInfo {
	std::string  id;                  // folder name, e.g. "zh-rTW"; "en" for the original text
	std::string  displayName;         // meta.json's display_name, or id when absent
	bool         slot[kDictSlotCount] = { false, false, false }; // which sets have it
};

// Languages actually present on disk (folders under each slot's resolved root).
//  - "en" is ALWAYS first and needs no folder (it is the no-dictionary fallback, not discoverable by scanning).
//  - The result is the UNION across the three sets; a language present only for PoE1 makes
//    PoE2 pass the original text through, as for "en".
std::vector<LocaleInfo> ListInstalledLocales(const std::wstring& exeDir, const LauncherConfig& cfg);

// Index into `locales` for `want`, falling back to zh-rTW then en (index 0). Never out of range.
int PickLocaleIndex(const std::vector<LocaleInfo>& locales, const std::wstring& want);

// Is `path` the same folder as `parentDir` or inside it? Case-insensitive; "D:\app\Data2" is NOT
// inside "D:\app\Data". Exposed for the selftest.
bool PathIsInside(const std::wstring& parentDir, const std::wstring& path);

// Copy one slot's built-in dictionaries into `dest`, producing the layout ResolveDictDir expects.
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
// The launcher's save. The new interface (another process) owns the Modern* keys and writes them
// while the launcher is open, so this takes those keys from disk first instead of writing back stale copies.
void SaveLauncherConfigKeepModern(const std::wstring& iniPath, LauncherConfig& cfg);
// POB_ZH_DICT_SWITCHES for the engine: "file=1;file=0", one item per
// kOptionalDicts row ("=0" = skip that file). SetEngineEnv always writes it.
std::wstring DictSwitchesEnv(const LauncherConfig& cfg);
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

// Background images the user dropped into PobTools\Backgrounds\ (png/jpg/jpeg/
// webp), file names only; the folder is created on first use so "open folder"
// always has somewhere to go. Update packages never touch PobTools\.
std::wstring BackgroundsDir(const std::wstring& exeDir);
std::vector<std::wstring> ListAvailableBackgrounds(const std::wstring& exeDir, bool includeVideo = false);
// Absolute path for a configured background, empty when unset or missing.
std::wstring ResolveBackgroundPath(const std::wstring& exeDir, const std::wstring& file);

// Full path to the font to load: the requested fontFile if present, otherwise
// falls back to the Noto default, then FZ_ZY, then any Fonts\*.ttf.
std::wstring ResolveFontPath(const std::wstring& exeDir, const std::wstring& fontFile);

// Full paths of every other shipped font (every Fonts\*.ttf except the one `fontFile` resolves to),
// merged into the ImGui atlas as glyph fallbacks (Noto Sans TC lacks simplified-only characters).
std::vector<std::wstring> FallbackFontPaths(const std::wstring& exeDir, const std::wstring& fontFile);

// Convenience for the editor/atlas entry points: read the configured font from
// <exeDir>\pob-zh.ini and resolve it to a full path (with the same fallbacks).
std::wstring ResolveConfiguredFontPath(const std::wstring& exeDir);
