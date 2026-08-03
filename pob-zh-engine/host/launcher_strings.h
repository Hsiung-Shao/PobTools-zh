// Launcher UI string tables (UTF-8 literals; this file must stay UTF-8 encoded).
#pragma once

#include <string>

struct LauncherStrings {
	const char* title;
	const char* subtitle;
	const char* language;
	const char* gameVersion;
	const char* poe1;
	const char* poe2;
	const char* detected;         // card status: install found
	const char* missing;          // card status: install missing
	const char* notFoundPoe1;     // tooltip on a disabled POE1 card
	const char* notFoundPoe2;     // tooltip on a disabled POE2 card
	const char* noneFound;        // shown when both installs are missing
	const char* returnAfterExit;
	const char* launch;           // per-row launch button (short)
	const char* editor;           // open the translation editor
	const char* filterEditor;     // open the loot-filter editor
	const char* atlasPlanner;     // open the atlas passive-tree planner
	const char* timelessJewel;    // open the timeless-jewel calculator
	const char* gamesSection;     // wide-layout section labels
	const char* toolsSection;
	const char* linksSection;
	const char* about;            // about dialog: button + title
	const char* changelog;        // version-history dialog: button + title
	const char* aboutBody;        // about dialog: description / attribution (multi-line)
	const char* support;          // link board: support / buy-me-a-coffee label
	const char* discord;          // link board: community Discord label
	const char* close;            // about dialog: close button
	const char* font;             // status bar: font picker label
	const char* updateAvailable;  // status bar: orange button prefix ("New version v")
	const char* updateNow;        // status bar: orange button suffix ("click to update")
	const char* updateDownloading;// status bar: byte-progress prefix
	const char* updatePreparing;  // status bar: extract/validate phase
	const char* updateRestarting; // status bar: stage ready, about to relaunch
	const char* updateFailed;     // status bar: error prefix (detail appended)
	const char* updateRetry;      // status bar: small retry button
	const char* updateTransDone;  // status bar: translation pack applied notice
	const char* updateCheck;      // header: manual "check for updates" button
	const char* updateCheckTip;   // header: tooltip explaining the automatic cadence
	const char* updateChecking;   // header: check in flight
	const char* updateUpToDate;   // header: manual check found nothing new
	// --- appended for the tabbed layout. New fields go at the END: both tables
	// are positional aggregate initialisers, so inserting in the middle shifts
	// every later string silently.
	const char* tabHome;          // tab: the launch / tools / links page
	const char* tabSettings;      // tab: settings
	const char* sectionInterface; // settings group: language + font
	const char* sectionLaunch;    // settings group: what happens when POB starts
	const char* exitModeClose;    // radio: launcher closes with POB
	const char* exitModeKeepOpen; // radio: launcher stays up, POB can be re-launched
	const char* startupTabLabel;  // settings: which tab to open on
	const char* pobRunning;       // home: "POB running: N"
	const char* pobSameGameWarn;  // home: two instances share one install's saves
	const char* updateBlockedTip; // header: why the update button is disabled
};

// Field count, used by launcher_ui.cpp to prove every string is fed to the glyph
// atlas. ImGui draws a missing glyph as '?' with no warning of any kind, so a
// string that never reaches the atlas is only discovered from a screenshot.
inline constexpr size_t kLauncherStringsFields = sizeof(LauncherStrings) / sizeof(const char*);

inline constexpr LauncherStrings STR_ZHTW = {
	u8"Path of Building 啟動器",
	u8"零污染技術，自動注入繁體中文翻譯與介面",
	u8"介面語言",
	u8"遊戲版本",
	u8"Path of Exile 1",
	u8"Path of Exile 2",
	u8"已偵測到",
	u8"未找到",
	u8"未找到 PoE1 版 POB（pob-zh.exe 旁任一資料夾內含 Launch.lua 即可，名稱不限）",
	u8"未找到 PoE2 版 POB（資料夾名稱需含 PoE2，內含 Launch.lua）",
	u8"未偵測到任何 POB，請將 POB 資料夾放在 pob-zh.exe 旁",
	u8"POB 關閉後回到啟動器",
	u8"啟動",
	u8"翻譯編輯器(Beta)",
	u8"過濾器編輯器",
	u8"輿圖策略",
	u8"軍團珠寶",
	u8"遊戲",
	u8"工具",
	u8"外部連結",
	u8"關於",
	u8"版本資訊",
	u8"PobTools — Path of Building 繁體中文化工具\n非官方粉絲工具，與 Grinding Gear Games 無關。\n程式碼採 MIT 授權，基於 Path of Building Community 與 SimpleGraphic（皆 MIT）。\n若這個工具對你有幫助，歡迎請我喝杯咖啡，這是我持續維護的動力。",
	u8"請我喝杯咖啡(贊助作者)",
	u8"Discord 社群（意見與功能討論）",
	u8"關閉",
	u8"字型",
	u8"發現新版 v",
	u8"，點擊更新",
	u8"下載更新中 ",
	u8"準備更新檔案…",
	u8"更新完成，即將重新啟動…",
	u8"更新失敗：",
	u8"重試",
	u8"翻譯資料已更新至 v",
	u8"檢查更新",
	u8"自動檢查每天最多一次，且只在啟動時進行；按這裡立即強制檢查",
	u8"檢查更新中…",
	u8"已是最新版本",
	u8"主畫面",
	u8"設定",
	u8"介面",
	u8"啟動 POB 後",
	u8"關閉啟動器",
	u8"保持啟動器開啟（可再開一個 POB）",
	u8"啟動時顯示",
	u8"POB 執行中：",
	u8"同一個 POB 開了多個視窗；它們共用設定與流派檔，最後關閉的那個會蓋掉其他的",
	u8"POB 執行中無法更新：更新會替換 engine 資料夾裡的檔案，請先關閉所有 POB 視窗",
};

inline constexpr LauncherStrings STR_EN = {
	u8"Path of Building Launcher",
	u8"Zero-pollution auto-injected Traditional Chinese translation and UI",
	u8"Interface language",
	u8"Game version",
	u8"Path of Exile 1",
	u8"Path of Exile 2",
	u8"Detected",
	u8"Not found",
	u8"No PoE1 POB found (any folder with Launch.lua next to pob-zh.exe works)",
	u8"No PoE2 POB found (folder name must contain PoE2 and hold Launch.lua)",
	u8"No POB detected. Put the POB folder next to pob-zh.exe",
	u8"Return to launcher after POB exits",
	u8"Launch",
	u8"Translation editor (Beta)",
	u8"Filter editor",
	u8"Atlas strategy",
	u8"Timeless jewel",
	u8"Games",
	u8"Tools",
	u8"Links",
	u8"About",
	u8"Version history",
	u8"PobTools — Traditional Chinese localization for Path of Building.\nUnofficial fan-made tool, not affiliated with Grinding Gear Games.\nMIT-licensed, built on Path of Building Community and SimpleGraphic (both MIT).\nIf you find it useful, consider buying me a coffee — it keeps me going.",
	u8"Buy me a coffee",
	u8"Discord community (feedback & feature requests)",
	u8"Close",
	u8"Font",
	u8"New version v",
	u8", click to update",
	u8"Downloading update ",
	u8"Preparing update files...",
	u8"Update complete, restarting...",
	u8"Update failed: ",
	u8"Retry",
	u8"Translation data updated to v",
	u8"Check for updates",
	u8"Automatic checks run at most once a day, and only at startup. Click to check right now.",
	u8"Checking for updates...",
	u8"Up to date",
	u8"Home",
	u8"Settings",
	u8"Interface",
	u8"After launching POB",
	u8"Close the launcher",
	u8"Keep the launcher open (POB can be launched again)",
	u8"Tab shown at startup",
	u8"POB running: ",
	u8"Several windows of the same POB are open; they share its settings and build files, and the last one closed overwrites the others",
	u8"Cannot update while POB is running: the update replaces files in the engine folder. Close every POB window first.",
};

// Only Traditional Chinese (zh-rTW) and English are offered. "en" (and any
// other/unknown locale) uses the English UI; the engine then finds no matching
// dictionary and passes POB text through untranslated.
inline const LauncherStrings& StringsFor(const std::wstring& locale, bool /*koreanOk*/ = true)
{
	if (locale == L"zh-rTW") return STR_ZHTW;
	return STR_EN;
}
