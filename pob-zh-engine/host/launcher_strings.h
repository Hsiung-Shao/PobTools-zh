// Launcher UI string tables (UTF-8 literals; this file must stay UTF-8 encoded).
#pragma once

#include <cstddef>
#include <string>

// ONE list, five expansions: the struct fields, both translation tables, the
// field-name array and the member-pointer array used to walk a LauncherStrings
// by index all come from here.
//
// Why: these used to be four hand-maintained parallel copies (struct, STR_ZHTW,
// STR_EN, and the glyph-atlas list in launcher_ui.cpp). A positional aggregate
// initialiser silently shifts every later string when one is inserted in the
// middle, and the static_assert added in v0.15.0 could only catch a MISSING
// entry, never a misordered one. With a single list there is nothing left to
// misorder.
//
//   X(field, zh-rTW, en)
//
// Appending is still the safe habit, but for a different reason now: the shipped
// Data\launcher\zh-rTW\launcher.json keys off the ENGLISH string, so reordering
// is harmless while EDITING an English string orphans that file's entry --
// re-export with --launcher-strings-export after any English change.
#define LAUNCHER_STRINGS(X)                                                                          \
	X(title,             u8"Path of Building 啟動器",                                                 \
	                     u8"Path of Building Launcher")                                              \
	X(subtitle,          u8"零污染技術，自動注入繁體中文翻譯與介面",                                  \
	                     u8"Zero-pollution auto-injected Traditional Chinese translation and UI")     \
	X(language,          u8"介面語言",                     u8"Interface language")                    \
	X(gameVersion,       u8"遊戲版本",                     u8"Game version")                          \
	X(poe1,              u8"Path of Exile 1",              u8"Path of Exile 1")                       \
	X(poe2,              u8"Path of Exile 2",              u8"Path of Exile 2")                       \
	/* card status: install found / missing */                                                       \
	X(detected,          u8"已偵測到",                     u8"Detected")                              \
	X(missing,           u8"未找到",                       u8"Not found")                             \
	/* tooltips on a disabled game card */                                                           \
	X(notFoundPoe1,      u8"未找到 PoE1 版 POB（pob-zh.exe 旁任一資料夾內含 Launch.lua 即可，名稱不限）", \
	                     u8"No PoE1 POB found (any folder with Launch.lua next to pob-zh.exe works)") \
	X(notFoundPoe2,      u8"未找到 PoE2 版 POB（資料夾名稱需含 PoE2，內含 Launch.lua）",              \
	                     u8"No PoE2 POB found (folder name must contain PoE2 and hold Launch.lua)")   \
	X(noneFound,         u8"未偵測到任何 POB，請將 POB 資料夾放在 pob-zh.exe 旁",                     \
	                     u8"No POB detected. Put the POB folder next to pob-zh.exe")                  \
	X(returnAfterExit,   u8"POB 關閉後回到啟動器",         u8"Return to launcher after POB exits")    \
	X(launch,            u8"啟動",                         u8"Launch")                                \
	/* tool buttons */                                                                               \
	X(editor,            u8"翻譯編輯器(Beta)",             u8"Translation editor (Beta)")             \
	X(filterEditor,      u8"過濾器編輯器",                 u8"Filter editor")                         \
	X(atlasPlanner,      u8"輿圖策略",                     u8"Atlas strategy")                        \
	X(timelessJewel,     u8"軍團珠寶",                     u8"Timeless jewel")                        \
	X(regexTool,         u8"Poe Regex",                    u8"Poe Regex")                              \
	/* wide-layout section labels */                                                                 \
	X(gamesSection,      u8"遊戲",                         u8"Games")                                 \
	X(toolsSection,      u8"工具",                         u8"Tools")                                 \
	X(linksSection,      u8"外部連結",                     u8"Links")                                 \
	X(about,             u8"關於",                         u8"About")                                 \
	X(changelog,         u8"版本資訊",                     u8"Version history")                       \
	X(aboutBody,         u8"PobTools — Path of Building 繁體中文化工具\n非官方粉絲工具，與 Grinding Gear Games 無關。\n程式碼採 MIT 授權，基於 Path of Building Community 與 SimpleGraphic（皆 MIT）。\n若這個工具對你有幫助，歡迎請我喝杯咖啡，這是我持續維護的動力。", \
	                     u8"PobTools — Traditional Chinese localization for Path of Building.\nUnofficial fan-made tool, not affiliated with Grinding Gear Games.\nMIT-licensed, built on Path of Building Community and SimpleGraphic (both MIT).\nIf you find it useful, consider buying me a coffee — it keeps me going.") \
	X(support,           u8"請我喝杯咖啡(贊助作者)",       u8"Buy me a coffee")                       \
	X(discord,           u8"Discord 社群（意見與功能討論）", u8"Discord community (feedback & feature requests)") \
	X(close,             u8"關閉",                         u8"Close")                                 \
	X(font,              u8"字型",                         u8"Font")                                  \
	/* updater status line */                                                                        \
	X(updateAvailable,   u8"發現新版 v",                   u8"New version v")                         \
	X(updateNow,         u8"，點擊更新",                   u8", click to update")                     \
	X(updateDownloading, u8"下載更新中 ",                  u8"Downloading update ")                   \
	X(updatePreparing,   u8"準備更新檔案…",                u8"Preparing update files...")             \
	X(updateRestarting,  u8"更新完成，即將重新啟動…",      u8"Update complete, restarting...")        \
	X(updateFailed,      u8"更新失敗：",                   u8"Update failed: ")                       \
	X(updateRetry,       u8"重試",                         u8"Retry")                                 \
	/* 版號後面沒有 v:翻譯資料走的是 data-<n>,不是 semver */                                        \
	X(updateTransDone,   u8"翻譯資料已更新至 ",            u8"Translation data updated to ")          \
	X(updateCheck,       u8"檢查更新",                     u8"Check for updates")                     \
	X(updateCheckTip,    u8"自動檢查每天最多一次，且只在啟動時進行；按這裡立即強制檢查",              \
	                     u8"Automatic checks run at most once a day, and only at startup. Click to check right now.") \
	X(updateChecking,    u8"檢查更新中…",                  u8"Checking for updates...")               \
	X(updateUpToDate,    u8"已是最新版本",                 u8"Up to date")                            \
	/* tabbed layout (v0.15.0) */                                                                    \
	X(tabHome,           u8"主畫面",                       u8"Home")                                  \
	X(tabSettings,       u8"設定",                         u8"Settings")                              \
	X(sectionInterface,  u8"介面",                         u8"Interface")                             \
	X(sectionLaunch,     u8"啟動 POB 後",                  u8"After launching POB")                   \
	X(exitModeClose,     u8"關閉啟動器",                   u8"Close the launcher")                    \
	X(exitModeKeepOpen,  u8"保持啟動器開啟（可再開一個 POB）",                                        \
	                     u8"Keep the launcher open (POB can be launched again)")                      \
	X(startupTabLabel,   u8"啟動時顯示",                   u8"Tab shown at startup")                  \
	X(pobRunning,        u8"POB 執行中：",                 u8"POB running: ")                         \
	X(pobSameGameWarn,   u8"同一個 POB 開了多個視窗；它們共用設定與流派檔，最後關閉的那個會蓋掉其他的", \
	                     u8"Several windows of the same POB are open; they share its settings and build files, and the last one closed overwrites the others") \
	X(updateBlockedTip,  u8"POB 執行中無法更新：更新會替換 engine 資料夾裡的檔案，請先關閉所有 POB 視窗", \
	                     u8"Cannot update while POB is running: the update replaces files in the engine folder. Close every POB window first.") \
	/* translation-data settings (v0.16.0) */                                                        \
	X(sectionTransData,  u8"翻譯資料",                     u8"Translation data")                     \
	X(transDataHint,     u8"三組字典可以各自指到安裝目錄以外的資料夾，POB 會讀你指定的那一份；更新只會寫入安裝目錄，碰不到它。留空就是用內建資料。", \
	                     u8"Each of the three dictionary sets can point at its own folder outside the install; POB reads the one you point at, and updates only ever write to the install folder, so they cannot touch it. Leave a field empty to use the built-in data.") \
	X(dataDirLabel,      u8"翻譯資料夾",                   u8"Data folder")                          \
	X(slotLauncher,      u8"啟動器介面",                   u8"Launcher UI")                          \
	X(browse,            u8"瀏覽…",                        u8"Browse...")                            \
	X(clearPath,         u8"清除",                         u8"Clear")                                \
	X(saveSettings,      u8"儲存設定",                     u8"Save settings")                        \
	X(settingsSaved,     u8"已儲存",                       u8"Saved")                                \
	X(saveSettingsHint,  u8"這一頁的設定改動都會立即寫入 pob-zh.ini，這個按鈕只是再存一次並確認。", \
	                     u8"Everything on this page is written to pob-zh.ini as you change it; this button just writes again and confirms.") \
	/* v0.28.0: the failure log. Named "問題紀錄" rather than "log" because the
	   only time anyone goes looking is when something is wrong, and that is what
	   we need them to send us. */                                                                \
	X(openLogFolder,     u8"開啟問題紀錄資料夾",           u8"Open the problem log folder")          \
	X(openLogFolderHint, u8"程式遇到問題時會把原因寫進這裡（每天一個檔）。回報問題時附上當天那一個檔最有幫助；一切正常時它會是空的。", \
	                     u8"Whenever something fails, the reason is written here (one file per day). Attaching today's file to a bug report is the most useful thing you can do; when nothing is wrong it stays empty.") \
	X(useSuggestion,     u8"改用這個",                     u8"Use this")                             \
	X(dataDirEmptyHint,  u8"留空 = 使用內建資料",          u8"Leave empty to use the built-in data")  \
	X(dataDirBuiltin,    u8"使用內建資料：",               u8"Using the built-in data: ")             \
	X(dataDirExternal,   u8"使用外部資料夾：",             u8"Using an external folder: ")            \
	X(dataDirMissing,    u8"找不到這個資料夾，暫時改用內建資料",                                      \
	                     u8"That folder does not exist; falling back to the built-in data")          \
	X(dataDirWrongShape, u8"這是語系資料夾本身，請改選它的上一層",                                    \
	                     u8"This is the locale folder itself; choose the folder above it")           \
	X(dataDirTooShallow, u8"這是上一層，請改選裡面對應的那個資料夾",                                  \
	                     u8"This is one level too high; choose the matching folder inside it")        \
	X(dataDirNoDict,     u8"這個資料夾裡沒有翻譯字典。按「複製…」把內建資料複製過去就會是正確的結構。", \
	                     u8"No dictionaries in that folder. Use \"Copy...\" to put the built-in data there and get the right layout.") \
	X(dataDirInside,     u8"這個資料夾在安裝目錄底下，更新時會被覆蓋；請改放到安裝目錄以外",          \
	                     u8"This folder is inside the install directory, so an update will overwrite it. Move it somewhere else.") \
	X(dataDirStale,      u8"外部副本的 meta.json 沒有列出這些檔案，它們不會被載入：",                 \
	                     u8"The external copy's meta.json does not list these files, so they are not loaded: ") \
	X(dataDirRestart,    u8"（啟動器本身的文字要重開啟動器才會換）",                                  \
	                     u8"(the launcher's own labels change on the next launcher start)")           \
	X(copyBuiltin,       u8"複製…",                        u8"Copy...")                              \
	X(copyDone,          u8"已複製 ",                      u8"Copied ")                              \
	X(copyDoneSuffix,    u8" 個檔案",                      u8" file(s)")                             \
	X(copyOverwrite,     u8"這個資料夾裡已經有翻譯字典了。繼續會覆蓋掉裡面的修改。",                  \
	                     u8"That folder already holds dictionaries. Continuing overwrites the edits in them.") \
	X(overwriteConfirm,  u8"覆蓋",                         u8"Overwrite")                            \
	X(cancel,            u8"取消",                         u8"Cancel")                               \
	/* v0.19.0:翻譯資料自成一條發佈線,不再跟著程式更新走,「一併」已不成立 */                        \
	X(transUpdateLabel,  u8"自動更新翻譯資料",             u8"Automatically update translation data") \
	X(transUpdateOn,     u8"是：新賽季的詞綴與物品翻譯自動跟上",                                      \
	                     u8"Yes: new-league affix and item translations arrive automatically")        \
	X(transUpdateOff,    u8"否：我自己在改翻譯，不要覆蓋我的檔案",                                    \
	                     u8"No: I edit the translations myself, do not overwrite my files")           \
	X(transApplyNow,     u8"立即套用一次",                 u8"Apply once now")                       \
	/* 兩個版號了:程式一個、翻譯資料一個,設定頁要說得出目前是哪一份 */                              \
	X(transDataVersion,  u8"目前翻譯資料版本：",           u8"Translation data version: ")            \
	X(transDataUnstamped, u8"未標示（v0.18.0 以前的安裝）",                                           \
	                     u8"not stamped (installed before v0.19.0)")                                  \
	/* v1.1.0:大陸使用者連 GitHub 常需代理;空值＝自動跟隨系統代理 */                                \
	X(sectionNetwork,    u8"網路",                         u8"Network")                               \
	X(proxyLabel,        u8"HTTP 代理",                    u8"HTTP proxy")                            \
	X(proxyEmptyHint,    u8"留空＝自動跟隨系統代理",       u8"empty = follow the system proxy")       \
	X(proxyNote,         u8"更新檢查、翻譯資料與圖示下載都會經過這個代理（例：127.0.0.1:7890）。"     \
	                     u8"留空時自動跟隨系統代理，開著 Clash / V2Ray 類工具即可直接使用。",         \
	                     u8"Update checks, translation data and icon downloads go through this "      \
	                     u8"proxy (e.g. 127.0.0.1:7890). When empty the system proxy is followed "    \
	                     u8"automatically.")                                                          \
	X(homeExternalData,  u8"翻譯資料：外部資料夾",         u8"Translation data: external folder")     \
	X(useBuiltin,        u8"切回內建",                     u8"Use built-in")                          \
	/* font installation + coverage (v0.16.0) */                                                     \
	X(installFont,       u8"安裝字型…",                    u8"Install a font...")                     \
	X(fontInstalled,     u8"已安裝並選用 ",                u8"Installed and selected ")               \
	X(fontAlreadyThere,  u8"Fonts 資料夾已經有同名字型，改用現有的那個",                              \
	                     u8"A font with that name is already in Fonts; using the existing one")       \
	X(fontCff,           u8"這個字型是 OpenType/CFF 輪廓，啟動器讀不了（POB 內文可以）。請改用 TrueType 的 .ttf。", \
	                     u8"That font uses OpenType/CFF outlines, which the launcher cannot read (POB itself can). Use a TrueType .ttf instead.") \
	X(fontNotAFont,      u8"這不是可以用的字型檔",         u8"That is not a usable font file")        \
	X(fontCopyFailed,    u8"複製字型失敗",                 u8"Could not copy the font")               \
	X(fontMissingGlyphs, u8"目前的字型畫不出這個語言的文字",                                          \
	                     u8"The current font cannot draw this language")                              \
	X(fontMissingHere,   u8"目前的字型畫不出這些字：",     u8"The current font is missing: ")          \
	/* 圖集裝不下完整中文字集 (v0.21.0)。分頁標題會顯示 POB 的專案名稱,那是任意文字,           \
	   所以整個中文字集都要進圖集;但圖集大小受顯示卡材質上限與顯示縮放兩者夾擊,               \
	   放不下時只能砍掉再說 —— 而 ImGui 對缺字是靜默畫問號,不講就沒人知道為什麼 */            \
	X(fontAtlasTrimmed,  u8"顯示卡的材質上限放不下完整中文字集，部分罕用字（例如專案名稱裡的字）會顯示為問號。降低顯示縮放比例可以避免。", \
	                     u8"The full Chinese character set does not fit this GPU's texture limit, so rare characters (in build names, for example) show as '?'. A lower display scaling avoids it.") \
	/* window mode (v0.17.0) */                                                                      \
	X(sectionWindow,     u8"視窗方式",                     u8"Windows")                               \
	X(winModeSeparate,   u8"各自獨立的視窗",               u8"Separate desktop windows")              \
	X(winModeTabbed,     u8"在啟動器裡開成分頁",           u8"As tabs inside this launcher")          \
	X(winModeHint,       u8"分頁模式下，POB 與工具會嵌在這個視窗裡；POB 本身照常執行，資料夾不會被動到。", \
	                     u8"In tabbed mode POB and the tools sit inside this window. POB itself runs normally and nothing is written to its folder.") \
	X(winModeRestart,    u8"改完要重開啟動器才會生效",     u8"Restart the launcher for this to take effect") \
	/* 只解壓了主檔包的防呆 (v0.19.0)。這個情境沒有任何錯誤訊息:程式正常啟動、                       \
	   啟動器介面照樣是中文(編譯進 exe 的字串表兜底),只有 POB 全英文 —— 所以                       \
	   偵測不到字典時要主動說出來,不能等使用者自己想通 */                                            \
	X(noDictBanner,      u8"這份安裝沒有翻譯資料，POB 會顯示英文原文。主程式更新包不含字典，需要另外下載一次。", \
	                     u8"This install has no translation data, so POB shows the original English. The program update pack does not carry the dictionaries; they are downloaded separately.") \
	X(noDictDownload,    u8"立即下載翻譯資料",             u8"Download translation data now")           \
	/* font: apply the selected TTF to ASCII in the POB window too (v0.22.0) */                      \
	X(fontApplyAllChk,   u8"POB 視窗的英文與數字也用此字型",                                          \
	                     u8"Use this font for letters & digits in POB too")                          \
	X(fontApplyAllTip,   u8"開啟後 POB 視窗內中英數共用同一個字型。等寬欄位（主控台、物品原文編輯）一律維持原生等寬字型。切換後需重新啟動 POB 視窗才會生效。", \
	                     u8"When on, Chinese, letters and digits in the POB window share the selected font. Monospaced fields (console, raw item editing) keep the native fixed-width font. Takes effect the next time the POB window starts.") \
	/* launcher-only zoom + remembered window size (v1.3.0). ASCII 'x' between the  \
	   width and height fields on purpose: U+00D7 is not in every shipped font. */    \
	X(fontSizeLabel,     u8"字體大小",                       u8"Font size")                              \
	X(fontSizeHint,      u8"只影響啟動器與工具視窗，POB 本身不受影響",                                    \
	                     u8"Affects only the launcher and its tool windows; POB itself is unchanged")   \
	X(resetDefault,      u8"恢復預設",                       u8"Reset to default")                       \
	X(winSizeLabel,      u8"視窗大小",                       u8"Window size")                            \
	X(winSizeHint,       u8"也可以直接拖曳視窗邊緣調整，大小會記住",                                      \
	                     u8"You can also drag the window edge; the size is remembered")

struct LauncherStrings {
#define PT_LS_FIELD(name, zh, en) const char* name;
	LAUNCHER_STRINGS(PT_LS_FIELD)
#undef PT_LS_FIELD
};

inline constexpr LauncherStrings STR_ZHTW = {
#define PT_LS_ZH(name, zh, en) zh,
	LAUNCHER_STRINGS(PT_LS_ZH)
#undef PT_LS_ZH
};

inline constexpr LauncherStrings STR_EN = {
#define PT_LS_EN(name, zh, en) en,
	LAUNCHER_STRINGS(PT_LS_EN)
#undef PT_LS_EN
};

// Field count, used by launcher_ui.cpp to prove every string is fed to the glyph
// atlas. ImGui draws a missing glyph as '?' with no warning of any kind, so a
// string that never reaches the atlas is only discovered from a screenshot.
inline constexpr size_t kLauncherStringsFields = sizeof(LauncherStrings) / sizeof(const char*);

// Member pointers, index-aligned with the struct. Walking the fields by index is
// what lets the JSON overlay and the selftests treat LauncherStrings as a list
// instead of writing a 49-way switch. Member pointers rather than a
// reinterpret_cast over the struct: same convenience, no aliasing question.
inline constexpr const char* LauncherStrings::* const kLauncherStringMembers[] = {
#define PT_LS_MEMBER(name, zh, en) &LauncherStrings::name,
	LAUNCHER_STRINGS(PT_LS_MEMBER)
#undef PT_LS_MEMBER
};

// Field names, index-aligned with the above. Diagnostics only -- the JSON keys
// off the English string, not off these.
inline constexpr const char* const kLauncherStringNames[] = {
#define PT_LS_NAME(name, zh, en) #name,
	LAUNCHER_STRINGS(PT_LS_NAME)
#undef PT_LS_NAME
};

static_assert(sizeof(kLauncherStringMembers) / sizeof(kLauncherStringMembers[0]) ==
                  kLauncherStringsFields,
              "member-pointer table must cover every LauncherStrings field");
static_assert(sizeof(kLauncherStringNames) / sizeof(kLauncherStringNames[0]) ==
                  kLauncherStringsFields,
              "name table must cover every LauncherStrings field");

// Only Traditional Chinese (zh-rTW) and English are offered. "en" (and any
// other/unknown locale) uses the English UI; the engine then finds no matching
// dictionary and passes POB text through untranslated.
inline const LauncherStrings& StringsFor(const std::wstring& locale, bool /*koreanOk*/ = true)
{
	if (locale == L"zh-rTW") return STR_ZHTW;
	return STR_EN;
}
