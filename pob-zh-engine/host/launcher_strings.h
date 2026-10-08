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
	X(language,          u8"介面語言",                     u8"Interface language")                    \
	X(gameVersion,       u8"遊戲版本",                     u8"Game version")                          \
	X(poe1,              u8"Path of Exile 1",              u8"Path of Exile 1")                       \
	X(poe2,              u8"Path of Exile 2",              u8"Path of Exile 2")                       \
	/* card status: install found / missing */                                                       \
	X(detected,          u8"已偵測到",                     u8"Detected")                              \
	X(missing,           u8"未找到",                       u8"Not found")                             \
	X(returnAfterExit,   u8"POB 關閉後回到啟動器",         u8"Return to launcher after POB exits")    \
	X(launch,            u8"啟動",                         u8"Launch")                                \
	/* tool buttons */                                                                               \
	X(editor,            u8"翻譯編輯器(Beta)",             u8"Translation editor (Beta)")             \
	X(filterEditor,      u8"過濾器編輯器",                 u8"Filter editor")                         \
	X(atlasPlanner,      u8"輿圖策略",                     u8"Atlas strategy")                        \
	X(timelessJewel,     u8"軍團珠寶",                     u8"Timeless jewel")                        \
	X(regexTool,         u8"Poe Regex",                    u8"Poe Regex")                              \
	X(warehouseTool,     u8"倉庫收益",                     u8"Stash tracker")                         \
	X(modernUiTool,      u8"新介面(Beta)",                 u8"Modern UI (Beta)")                      \
	X(modernUiTip,       u8"以新版介面開啟建置(測試中)",   u8"Open a build in the new interface (preview)") \
	X(uiModeLabel,       u8"預設介面",                     u8"Default interface")                     \
	X(uiModeClassic,     u8"經典",                         u8"Classic")                               \
	X(uiModeModern,      u8"新介面(Beta)",                 u8"New interface (Beta)")                  \
	X(uiModeUnavailable, u8"找不到新介面的頁面檔(ui\\index.html),「啟動」一律開經典視窗。", \
	                     u8"The new interface's page (ui\\index.html) is missing; Launch always opens the classic window.") \
	X(uiModeBrowser,     u8"按「啟動」時開哪一種視窗(PoE1 與 PoE2 都適用)。這台電腦沒有 WebView2(例如 macOS / Linux 的 CrossOver、Wine),新介面會在系統瀏覽器開啟;按頁面右上角「結束」或在這裡結束,只關分頁的話約 20 秒後結束。", \
	                     u8"Which window the Launch button opens, for PoE1 and PoE2 alike. This computer has no WebView2 (CrossOver / Wine on macOS or Linux), so the new interface opens in the system browser; end it with the page's End button or here. Closing only the tab ends it about 20 seconds later.") \
	X(modernBrowserRunning, u8"新介面(瀏覽器)執行中：", u8"New interface (browser) running: ") \
	X(modernBrowserOpen, u8"開啟頁面",                     u8"Open page")                             \
	X(modernBrowserCopyUrl, u8"複製網址",                  u8"Copy address")                          \
	X(modernBrowserStop, u8"結束",                         u8"End")                                   \
	/* Phase 3: the new interface's compatibility gate failed against this POB
	   version; the classic window opened instead and the button waits for the
	   next POB update. %s = POB version, %d = failed probe count. */                              \
	X(modernGateBanner,  u8"新介面與這版 POB(%s)不相容,已改開經典視窗;POB 更新後會再試", u8"The new interface is not compatible with this POB (%s); the classic window opened instead. It will try again after POB updates") \
	X(modernGateTip,     u8"%d 項相容性檢查失敗:\n%s", u8"%d compatibility checks failed:\n%s") \
	/* wide-layout section labels */                                                                 \
	X(gamesSection,      u8"遊戲",                         u8"Games")                                 \
	X(toolsSection,      u8"工具",                         u8"Tools")                                 \
	X(linksSection,      u8"外部連結",                     u8"Links")                                 \
	X(about,             u8"關於",                         u8"About")                                 \
	X(changelog,         u8"版本資訊",                     u8"Version history")                       \
	X(aboutBody,         u8"PobTools — Path of Building 繁體中文化工具\n非官方粉絲工具，與 Grinding Gear Games 無關。\n程式碼採 MIT 授權，基於 Path of Building Community 與 SimpleGraphic（皆 MIT）。\n若這個工具對你有幫助，歡迎請我喝杯咖啡，這是我持續維護的動力。", \
	                     u8"PobTools — Traditional Chinese localization for Path of Building.\nUnofficial fan-made tool, not affiliated with Grinding Gear Games.\nMIT-licensed, built on Path of Building Community and SimpleGraphic (both MIT).\nIf you find it useful, consider buying me a coffee — it keeps me going.") \
	X(support,           u8"請我喝杯咖啡",                 u8"Buy me a coffee")                       \
	X(discord,           u8"Discord 社群", u8"Discord community (feedback & feature requests)") \
	X(close,             u8"關閉",                         u8"Close")                                 \
	X(font,              u8"字型",                         u8"Font")                                  \
	/* updater status line */                                                                        \
	X(updateAvailable,   u8"發現新版 v",                   u8"New version v")                         \
	X(updateNow,         u8"，點擊更新",                   u8", click to update")                     \
	X(updatePreparing,   u8"準備更新檔案…",                u8"Preparing update files...")             \
	X(updateRestarting,  u8"更新完成，即將重新啟動…",      u8"Update complete, restarting...")        \
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
	X(sectionLaunch,     u8"啟動 POB 後",                  u8"After launching POB")                   \
	X(exitModeClose,     u8"關閉啟動器",                   u8"Close the launcher")                    \
	X(exitModeKeepOpen,  u8"保持啟動器開啟（可再開一個 POB）",                                        \
	                     u8"Keep the launcher open (POB can be launched again)")                      \
	X(startupTabLabel,   u8"啟動時顯示",                   u8"Tab shown at startup")                  \
	X(pobSameGameWarn,   u8"同一個 POB 開了多個視窗；它們共用設定與流派檔，最後關閉的那個會蓋掉其他的", \
	                     u8"Several windows of the same POB are open; they share its settings and build files, and the last one closed overwrites the others") \
	X(updateBlockedTip,  u8"POB 執行中無法更新：更新會替換 engine 資料夾裡的檔案，請先關閉所有 POB 視窗", \
	                     u8"Cannot update while POB is running: the update replaces files in the engine folder. Close every POB window first.") \
	/* translation-data settings (v0.16.0) */                                                        \
	X(sectionTransData,  u8"翻譯資料",                     u8"Translation data")                     \
	X(dataDirLabel,      u8"翻譯資料夾",                   u8"Data folder")                          \
	X(slotLauncher,      u8"啟動器介面",                   u8"Launcher UI")                          \
	X(browse,            u8"瀏覽…",                        u8"Browse...")                            \
	/* v0.28.0: the failure log. Named "問題紀錄" rather than "log" because the
	   only time anyone goes looking is when something is wrong, and that is what
	   we need them to send us. */                                                                \
	X(openLogFolderHint, u8"程式遇到問題時會把原因寫進這裡（每天一個檔）。回報問題時附上當天那一個檔最有幫助；一切正常時它會是空的。", \
	                     u8"Whenever something fails, the reason is written here (one file per day). Attaching today's file to a bug report is the most useful thing you can do; when nothing is wrong it stays empty.") \
	X(useSuggestion,     u8"改用這個",                     u8"Use this")                             \
	X(dataDirEmptyHint,  u8"留空 = 使用內建資料",          u8"Leave empty to use the built-in data")  \
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
	X(copyBuiltin,       u8"複製到…",                        u8"Copy...")                              \
	X(copyDone,          u8"已複製 ",                      u8"Copied ")                              \
	X(copyDoneSuffix,    u8" 個檔案",                      u8" file(s)")                             \
	X(copyOverwrite,     u8"這個資料夾裡已經有翻譯字典了。繼續會覆蓋掉裡面的修改。",                  \
	                     u8"That folder already holds dictionaries. Continuing overwrites the edits in them.") \
	X(overwriteConfirm,  u8"覆蓋",                         u8"Overwrite")                            \
	X(cancel,            u8"取消",                         u8"Cancel")                               \
	/* v0.19.0:翻譯資料自成一條發佈線,不再跟著程式更新走,「一併」已不成立 */                        \
	X(transUpdateLabel,  u8"自動更新翻譯資料",             u8"Automatically update translation data") \
	X(transApplyNow,     u8"立即套用一次",                 u8"Apply once now")                       \
	/* 兩個版號了:程式一個、翻譯資料一個,設定頁要說得出目前是哪一份 */                              \
	X(transDataUnstamped, u8"未標示（v0.18.0 以前的安裝）",                                           \
	                     u8"not stamped (installed before v0.19.0)")                                  \
	/* v1.1.0:大陸使用者連 GitHub 常需代理;空值＝自動跟隨系統代理 */                                \
	X(proxyLabel,        u8"HTTP 代理",                    u8"HTTP proxy")                            \
	X(proxyEmptyHint,    u8"留空＝自動跟隨系統代理",       u8"empty = follow the system proxy")       \
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
	X(fontMissingHere,   u8"目前的字型畫不出這些字：",     u8"The current font is missing: ")          \
	/* 圖集裝不下完整中文字集 (v0.21.0)。分頁標題會顯示 POB 的專案名稱,那是任意文字,           \
	   所以整個中文字集都要進圖集;但圖集大小受顯示卡材質上限與顯示縮放兩者夾擊,               \
	   放不下時只能砍掉再說 —— 而 ImGui 對缺字是靜默畫問號,不講就沒人知道為什麼 */            \
	X(fontAtlasTrimmed,  u8"顯示卡的材質上限放不下完整中文字集，部分罕用字（例如專案名稱裡的字）會顯示為問號。降低顯示縮放比例可以避免。", \
	                     u8"The full Chinese character set does not fit this GPU's texture limit, so rare characters (in build names, for example) show as '?'. A lower display scaling avoids it.") \
	/* window mode (v0.17.0) */                                                                      \
	X(sectionWindow,     u8"視窗方式",                     u8"Windows")                               \
	X(winModeRestart,    u8"改完要重開啟動器才會生效",     u8"Restart the launcher for this to take effect") \
	/* 只解壓了主檔包的防呆 (v0.19.0)。這個情境沒有任何錯誤訊息:程式正常啟動、                       \
	   啟動器介面照樣是中文(編譯進 exe 的字串表兜底),只有 POB 全英文 —— 所以                       \
	   偵測不到字典時要主動說出來,不能等使用者自己想通 */                                            \
	X(noDictDownload,    u8"立即下載翻譯資料",             u8"Download translation data now")           \
	/* font: apply the selected TTF to ASCII in the POB window too (v0.22.0) */                      \
	X(fontApplyAllChk,   u8"POB 視窗的英文與數字也用此字型",                                          \
	                     u8"Use this font for letters & digits in POB too")                          \
	X(fontApplyAllTip,   u8"開啟後 POB 視窗內中英數共用同一個字型。等寬欄位（主控台、物品原文編輯）一律維持原生等寬字型。切換後需重新啟動 POB 視窗才會生效。", \
	                     u8"When on, Chinese, letters and digits in the POB window share the selected font. Monospaced fields (console, raw item editing) keep the native fixed-width font. Takes effect the next time the POB window starts.") \
	/* launcher-only zoom + remembered window size (v1.3.0). ASCII 'x' between the  \
	   width and height fields on purpose: U+00D7 is not in every shipped font. */    \
	X(fontSizeLabel,     u8"字體大小",                       u8"Font size")                              \
	X(fontSizeHint,      u8"整個啟動器與工具視窗一起縮放，POB 不受影響",                                    \
	                     u8"Affects only the launcher and its tool windows; POB itself is unchanged")   \
	X(resetDefault,      u8"恢復預設",                       u8"Reset to default")                       \
	X(winSizeLabel,      u8"視窗大小",                       u8"Window size")                            \
	X(winSizeHint,       u8"也可以直接拖曳視窗邊緣調整，大小會記住",                                      \
	                     u8"You can also drag the window edge; the size is remembered")                \
	X(winOpacityLabel,   u8"POB 面板不透明度",               u8"POB panel opacity")                      \
	X(winOpacityHint,    u8"側欄、上方工具列與天賦頁底部工具列的底色與分隔線會淡出，0% 時完全消失，只剩文字與控制項疊在延伸到整個視窗的天賦樹上（其他分頁為斜紋底）；不會透出桌面。100% 為關閉，拖動時已開啟的 POB 視窗即時生效；僅 Windows 支援", \
	                     u8"The side bar, top bar and tree-tab toolbar backgrounds and separators fade out; at 0% they are gone and only text and controls remain over the passive tree, which then spans the whole window (striped background on other tabs). The desktop never shows through. 100% = off, open POB windows follow the slider live. Windows only") \
	X(tabAppearance,     u8"外觀",                           u8"Appearance")                             \
	X(bgLabel,           u8"背景圖片",                       u8"Background image")                       \
	X(bgOpenFolder,      u8"開啟資料夾",                     u8"Open folder")                            \
	X(bgBrightLabel,     u8"背景亮度",                       u8"Background brightness")                  \
	X(glassBlurLabel,    u8"霧面強度",                       u8"Frost strength")                         \
	X(treeBgLabel,       u8"天賦樹底圖",                     u8"Tree backdrop")                          \
	X(bgHint,            u8"把 PNG／JPG／WebP 放進 PobTools\\Backgrounds 資料夾就能選。圖片鋪滿整個 POB 視窗當最底層，天賦樹與各分頁畫在它上面；亮度是圖片壓暗的程度。霧面強度把側欄、工具列變成毛玻璃：面板底下的畫面先模糊再蓋上面板的淡色（面板不透明度）；0% 為不模糊。天賦樹底圖是節點底下那層深色底紋的不透明度，調低就能在天賦頁也看到背景圖片。全部即時生效", \
	                     u8"Drop PNG/JPG/WebP files into the PobTools\\Backgrounds folder to pick them here. The image fills the whole POB window as the bottom layer, with the tree and the tabs drawn over it; brightness dims the image. Frost strength turns the side bar and tool bars into frosted glass: what is under a panel is blurred, then the panel's tint (panel opacity) goes on top; 0% = no blur. Tree backdrop is the opacity of the dark tiled layer under the nodes; lower it to see the background image on the tree tab too. Everything applies live") \
	/* v1.4.0: the program-update line gets its own section. Off by default --
	   installing a new version closes the launcher and reopens it, and that is
	   not something to do to somebody who did not ask for it. */                                   \
	X(autoAppUpdate,     u8"啟動後自動安裝新版本",         u8"Install new versions at startup")       \
	X(autoAppUpdateHint, u8"PobTools 一開啟就檢查，有新版本就直接裝好並重新開啟，不必按右上角那個按鈕。只在剛啟動、還沒開過 POB 或任何工具時才會動手；有 POB 在跑時一律不動。翻譯資料的自動更新是下面那個獨立的設定。", \
	                     u8"PobTools checks when it opens and, if there is a new version, installs it and reopens itself instead of waiting for the button in the top right. It only ever acts right after startup, before POB or any tool has been opened, and never while a POB is running. Translation data has its own separate setting below.") \
	/* v1.6.0: the beta line. The hint has to carry all four facts -- what an
	   early build is, that it can be broken, that turning it back off does not
	   walk the install backwards, and that the dictionaries are the same ones
	   the stable line gets -- because nobody reads a hint twice. */            \
	X(betaChannel,       u8"參加 beta 測試（搶先版）", u8"Join the beta (early builds)") \
	X(betaChannelHint,   u8"打開之後，程式更新會抓還沒轉正式的搶先版。搶先版是測試用的，可能有還沒修好的問題；關掉之後不會退回舊版，會等正式版追上來。翻譯資料與正式版共用同一份。", \
	                     u8"With this on, program updates come from builds that have not been made official yet. Early builds are for testing and may have problems that are not fixed yet. Turning it back off does not downgrade: you keep what you have until the stable line catches up. The translation data is the same either way.") \
	/* Link-board labels that are NOT proper nouns. The rest of kLinks stays
	   hard-coded: site names (PoeDB, poe.ninja, FilterBlade...) read the same in
	   every language, and the Chinese-community links keep their Chinese labels
	   in every locale on purpose -- that IS what they are (user ruling). */      \
	X(linkOfficialSite,  u8"官方網站",                     u8"Official site")                         \
	X(linkTradePoe1,     u8"官方交易市集（PoE1）",         u8"Official trade (PoE1)")                 \
	X(linkTradePoe2,     u8"官方交易市集（PoE2）",         u8"Official trade (PoE2)")                 \
	X(linkDisenchant,    u8"拆粉查詢",                     u8"Disenchant lookup")                     \
	/* Column head over our own tools; the other two columns are headed PoE1 and
	   PoE2, which need no translation. */                                       \
	X(linkGroupTools,    u8"中文化工具",                   u8"Chinese localisation")                  \
	/* POB window performance (settings page): frame caps and the opt-in
	   performance log (host/pob_frame_cap.h, host/perf_log.h). */             \
	X(sectionPobPerf,    u8"POB 效能",                     u8"POB performance")                       \
	X(pobFpsForeground,  u8"POB 在前景時的畫面更新上限",   u8"POB frame limit while focused")         \
	X(pobFpsBackground,  u8"POB 在背景時的畫面更新上限",   u8"POB frame limit in the background")     \
	X(pobFpsUnlimited,   u8"不限",                         u8"No limit")                              \
	X(pobFpsHint,        u8"限制 POB 視窗每秒畫幾次，降低顯示卡與 CPU 負擔；改了會立刻套用到開著的 POB。畫面沒有變化時本來就不會重畫。覺得拖曳天賦樹不夠順再調高。", \
	                     u8"Limits how many times a second the POB window draws, which lowers GPU and CPU load; changes apply to running POB windows immediately. A picture that has not changed is not redrawn anyway. Raise it if dragging the passive tree feels choppy.") \
	X(perfLogChk,        u8"效能診斷記錄",                 u8"Performance diagnostics log")           \
	X(perfLogHint,       u8"排查 POB 卡頓或顯示卡、CPU 使用率過高時才開。下次開啟 POB 生效，記錄寫在問題紀錄資料夾的 perf-日期.log：用的是哪張顯示卡、在哪個頁面與狀態、每一段花了多少時間，關閉 POB 時會在最後附上各頁面的耗用排名。不含 build 內容或帳號資料。查完記得關掉。", \
	                     u8"Turn on only while chasing POB stutter or high GPU/CPU use. Takes effect the next time POB opens and writes perf-<date>.log into the problem log folder: which GPU is used, which page and state POB was in, and how long each part took, with a ranking of pages by cost appended when POB closes. No build contents or account data. Turn it off again when done.") \
	/* "Open in PoB" links (settings page, host/pob_protocol.h): pob:// and
	   pob2:// from poe.ninja / pobb.in and the other build sites. */            \
	X(sectionPobLinks,   u8"網站連結",                     u8"Website links")                         \
	X(pobProtocolChk,    u8"用 PobTools 開啟網站的「Open in PoB」連結", u8"Open websites' \"Open in PoB\" links with PobTools") \
	X(pobProtocolHint,   u8"讓 poe.ninja / pobb.in 等網站的「Open in PoB」開啟 PobTools(會取代官方安裝版 POB 的連結)。點連結時不顯示啟動器,直接依「預設介面」開啟中文化 POB 並匯入那個建置。只寫入目前使用者的登錄檔;取消勾選只移除指向這個 PobTools 的登記。", \
	                     u8"Lets the \"Open in PoB\" buttons on poe.ninja, pobb.in and other sites open PobTools (this replaces the official POB install's link handling). A link skips the launcher and opens the localised POB in your default interface with that build imported. Only the current user's registry is written; unticking removes only the registration that points at this PobTools.") \
	X(pobProtocolFail,   u8"無法變更連結登記(登錄檔寫入失敗),已還原勾選。", u8"Could not change the link registration (registry write failed); the setting was reverted.") \
	/* Launcher redesign (design system v1, 2026-10): header, banners, cards,
	   settings side navigation, appearance thumbnails, about card. */          \
	X(appSubtitle,       u8"Path of Building 繁體中文化", u8"Path of Building in Traditional Chinese") \
	X(updateTo,          u8"更新到 v%s",                   u8"Update to v%s")                         \
	X(updateDownloadingVer, u8"下載 v%s",                  u8"Downloading v%s")                       \
	X(transUpdatingHdr,  u8"更新翻譯資料",                 u8"Updating translation data")             \
	X(updateFailedPill,  u8"更新失敗",                     u8"Update failed")                         \
	X(updateSeeWhy,      u8"查看原因",                     u8"See why")                               \
	X(updateFailedTitle, u8"更新失敗：%s",                 u8"The update failed: %s")                 \
	X(updateFailedDesc,  u8"已保留目前版本。可以再試一次；一直失敗的話，回報時附上問題紀錄資料夾裡最新的檔案。", \
	                     u8"The current version was kept. Try again; if it keeps failing, report it with the newest file from the problem log folder.") \
	X(hangBannerTitle,   u8"POB 沒有回應已超過 20 秒",     u8"POB has not responded for over 20 seconds") \
	X(hangBannerDesc,    u8"原因已寫進問題紀錄。POB 恢復回應後這則會自動消失。", \
	                     u8"The reason was written to the problem log. This goes away once POB responds again.") \
	X(openLogShort,      u8"開啟問題紀錄",                 u8"Open the problem log")                  \
	X(extDataBannerTitle, u8"%s 翻譯正在使用外部資料夾",   u8"%s translations come from an external folder") \
	X(noDictTitle,       u8"這份安裝沒有翻譯資料，POB 會顯示英文", u8"This install has no translation data, so POB will show English") \
	X(noDictDesc,        u8"主程式更新包不含字典，需要另外下載一次。", u8"The program update pack carries no dictionaries; they are downloaded separately.") \
	X(moreBanners,       u8"還有 %d 則",                   u8"%d more")                               \
	X(uiModeShortClassic, u8"經典介面",                    u8"Classic interface")                     \
	X(uiModeShortModern, u8"新介面",                       u8"New interface")                         \
	X(pobRunningPill,    u8"執行中 ×%d",                   u8"Running ×%d")                           \
	X(notFoundPoe1Card,  u8"把裡面有 Launch.lua 的 POB 資料夾放在 pob-zh.exe 旁，回到這裡就會偵測到。", \
	                     u8"Put a POB folder with Launch.lua in it next to pob-zh.exe; it is detected when you come back here.") \
	X(notFoundPoe2Card,  u8"把資料夾名稱含 PoE2、裡面有 Launch.lua 的 POB 放在 pob-zh.exe 旁，回到這裡就會偵測到。", \
	                     u8"Put a POB folder whose name contains PoE2, with Launch.lua in it, next to pob-zh.exe; it is detected when you come back here.") \
	X(launchAnother,     u8"再開一個",                     u8"Open another")                          \
	X(launchBlockedUpdating, u8"更新下載中，完成後即可啟動", u8"An update is downloading; you can launch once it is done") \
	X(editorName,        u8"翻譯編輯器",                   u8"Translation editor")                    \
	X(warehouseBadge,    u8"PoE1 國際服",                  u8"PoE1 international")                    \
	X(tipFilter,         u8"編輯 .filter、預覽掉落外觀",   u8"Edit .filter files, preview how drops look") \
	X(tipAtlas,          u8"配點、聖甲蟲、星盤與每張圖成本", u8"Passives, scarabs, astrolabes and the cost of each map") \
	X(tipTimeless,       u8"查種子對天賦的影響、找想要的種子", u8"See what a seed changes, find the seed you want") \
	X(tipRegex,          u8"勾選詞綴，產生遊戲內搜尋字串", u8"Pick mods, get an in-game search string") \
	X(tipWarehouse,      u8"快照倉庫，算刷圖收益",         u8"Snapshot your stash, see what a map earned") \
	X(tipEditor,         u8"修改或補上 POB 的翻譯",        u8"Fix or fill in POB's translations")     \
	X(toolOpenTab,       u8"已開啟，點擊切換過去",         u8"Open; click to switch to it")           \
	X(linksMore,         u8"更多 %d 個…",                  u8"%d more…")                              \
	X(linksLess,         u8"收合",                         u8"Show fewer")                            \
	X(clSearchHint,      u8"搜尋版本紀錄…",                u8"Search the version history…")           \
	X(clCurrent,         u8"目前版本",                     u8"Current version")                       \
	X(clNoMatch,         u8"沒有符合的版本紀錄",           u8"No entries match")                      \
	X(navInterface,      u8"介面與字型",                   u8"Interface & font")                      \
	X(navLaunch,         u8"啟動與視窗",                   u8"Launch & windows")                      \
	X(navNetwork,        u8"網路與更新",                   u8"Network & updates")                     \
	X(navLog,            u8"問題紀錄",                     u8"Problem log")                           \
	X(settingsAutoSave,  u8"變更會立即儲存",               u8"Changes are saved immediately")         \
	X(uiModeHintShort,   u8"按「啟動」時開哪一種視窗；這版 POB 不相容時會自動開經典", \
	                     u8"What Launch opens; falls back to classic when this POB is not compatible") \
	X(fontHint,          u8"Fonts 資料夾裡的 .ttf",        u8".ttf files in the Fonts folder")        \
	X(fontApplyAllHint,  u8"下次開啟 POB 生效",            u8"Takes effect the next time POB opens")  \
	X(sliderDefault,     u8"預設",                         u8"Default")                               \
	X(winModeHintShort,  u8"分頁模式下 POB 與工具會嵌在啟動器裡", u8"In tabbed mode POB and the tools open inside the launcher") \
	X(winModeSeparateShort, u8"各自獨立",                  u8"Separate")                              \
	X(winModeTabbedShort, u8"啟動器分頁",                  u8"Launcher tabs")                         \
	X(exitModeTabbedNote, u8"分頁模式下不適用：POB 就開在啟動器裡", u8"Not used in tabbed mode: POB opens inside the launcher") \
	X(relaunchNow,       u8"立即重開",                     u8"Restart now")                           \
	X(proxyHintShort,    u8"更新檢查、翻譯資料與圖示下載都會經過這裡，例如 127.0.0.1:7890", \
	                     u8"Update checks, translation data and icon downloads go through it, e.g. 127.0.0.1:7890") \
	X(autoAppUpdateHintShort, u8"只在剛開啟、還沒開過 POB 或工具時才會安裝並重開", \
	                     u8"Only right after start, before any POB or tool has been opened") \
	X(betaHintShort,     u8"更新到還沒轉正式的搶先版；關掉後不會退回，會等正式版追上", \
	                     u8"Get builds before they are released; turning it off never downgrades, you wait for the release instead") \
	X(transUpdateHintShort, u8"自己在改翻譯的話請關掉，以免被新版蓋掉", \
	                     u8"Turn it off if you edit translations yourself, so an update does not overwrite them") \
	X(pobPerfNote,       u8"改了立即套用到開著的 POB",     u8"Applies to open POB windows right away") \
	X(pobFpsHintShort,   u8"畫面沒有變化時本來就不會重畫；拖曳天賦樹不夠順再調高", \
	                     u8"An unchanged picture is never redrawn; raise it if dragging the tree stutters") \
	X(perfLogHintShort,  u8"排查卡頓時才開，下次開啟 POB 生效；不含建置或帳號資料", \
	                     u8"Only for tracking down stutter; next POB start; no build or account data") \
	X(pobProtocolHintShort, u8"poe.ninja、pobb.in 的按鈕會直接開中文化 POB 並匯入建置；會取代官方安裝版 POB 的連結", \
	                     u8"Buttons on poe.ninja and pobb.in open the localised POB with the build imported; replaces the official POB's links") \
	X(pobProtoRegistered, u8"已登記",                      u8"Registered")                            \
	X(pobProtoNotRegistered, u8"未登記",                   u8"Not registered")                        \
	X(pobProtoOther,     u8"指向其他程式",                 u8"Points elsewhere")                      \
	X(pobProtocolWine,   u8"Wine / CrossOver 下不適用：網站開在系統瀏覽器，連結不會回到這裡", \
	                     u8"Not available under Wine / CrossOver: the sites open in the host browser, so links never reach here") \
	X(transDataNote,     u8"留空使用內建資料；更新只會寫入內建資料夾", u8"Empty uses the built-in data; updates only write the built-in folder") \
	X(dataPillBuiltin,   u8"內建",                         u8"Built-in")                              \
	X(dataPillExternal,  u8"外部資料夾",                   u8"External folder")                       \
	X(dataPillProblem,   u8"需要處理",                     u8"Needs attention")                       \
	X(logFolderLabel,    u8"問題紀錄資料夾",               u8"Problem log folder")                    \
	X(logFolderHint,     u8"回報問題時把裡面最新的檔案附上", u8"Attach the newest file in it when you report a problem") \
	X(lookTitle,         u8"POB 視窗外觀",                 u8"POB window look")                       \
	X(lookDesc,          u8"兩款遊戲各一組設定，拖動時已開啟的 POB 立即跟著變。", \
	                     u8"One set per game; open POB windows follow while you drag.") \
	X(bgAdd,             u8"加入圖片…",                    u8"Add an image…")                         \
	X(bgBuiltinShort,    u8"POB 內建底圖",                 u8"POB's own backdrop")                    \
	X(bgAddFailed,       u8"無法加入這張圖片",             u8"Could not add that image")              \
	X(bgAdded,           u8"已加入並選用 ",                u8"Added and selected ")                   \
	X(winOpacityHintShort, u8"側欄與工具列的底色；越低越能看到背景圖", \
	                     u8"The sidebar and toolbar backgrounds; lower shows more of the image") \
	X(bgBrightHint,      u8"背景圖太亮看不清字時調低",     u8"Lower it when the image makes text hard to read") \
	X(glassHint,         u8"讓背景圖模糊，文字更好讀",     u8"Blurs the image so text reads better")  \
	X(treeBgHint,        u8"天賦樹後面那張官方底圖的濃淡", u8"How strong the official art behind the passive tree is") \
	X(lookPreview,       u8"預覽",                         u8"Preview")                               \
	X(lookPreviewNote,   u8"示意圖：實際效果請看開著的 POB 視窗。這一頁的設定只在 Windows 有效。", \
	                     u8"Illustration only: an open POB window shows the real thing. These settings only work on Windows.") \
	X(lookWineNote,      u8"Wine / CrossOver 下 POB 視窗不支援這一頁的設定", u8"POB windows under Wine / CrossOver do not support the settings on this page") \
	X(aboutTagline,      u8"Path of Building 繁體中文化工具", u8"Traditional Chinese tools for Path of Building") \
	X(aboutAppVersion,   u8"程式版本",                     u8"Program version")                       \
	X(aboutBuiltOn,      u8"建置於 %s",                    u8"built %s")                              \
	X(aboutDetectedPob,  u8"偵測到的 POB",                 u8"Detected POB")                          \
	X(aboutReport,       u8"回報問題時",                   u8"When you report a problem")             \
	X(aboutReportHint,   u8"附上問題紀錄資料夾裡最新的檔案與上面這幾行版本", u8"Attach the newest problem-log file and the versions above") \
	X(copyVersionInfo,   u8"複製版本資訊",                 u8"Copy version info")                     \
	X(copiedToast,       u8"已複製",                       u8"Copied to the clipboard")               \
	X(launcherTab,       u8"啟動器",                       u8"Launcher")                              \
	X(copyOverwriteTitle, u8"覆蓋這個資料夾裡的翻譯？",    u8"Overwrite the translations in this folder?")

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
