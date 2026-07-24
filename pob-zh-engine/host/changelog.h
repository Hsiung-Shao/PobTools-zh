// Release history shown in the launcher's version-info dialog (scrollable).
// UPDATE THIS together with app_version.h on every release; newest entry on
// top. Formatting contract with launcher_ui.cpp: a line starting with 'v'+digit
// is a release header (accent color, extra gap above), a blank line separates
// releases, keep body lines short (~26 CJK chars) so they rarely wrap. The
// whole string is AddText'ed into the launcher glyph atlas, so any Chinese
// used here renders without extra font work.
#pragma once

inline constexpr const char* kChangelogText =
	u8"v0.4.0（2026-07-24）\n"
	u8"改進：POB 資料夾偵測全面放寬\n"
	u8"　- pob-zh.exe 旁任何內含 Launch.lua 的資料夾\n"
	u8"　　都會被偵測，名稱不限（官方名稱優先採用）。\n"
	u8"　- 資料夾名稱含 PoE2 視為 PoE2 版 POB。\n"
	u8"　- 支援把 pob-zh.exe 直接放進 POB 資料夾（同層）。\n"
	u8"新增：本「版本資訊」視窗。\n"
	u8"\n"
	u8"v0.3.0（2026-07-24）\n"
	u8"新增：內建自動更新\n"
	u8"　- 翻譯更新（如 0.4.0 -> 0.4.1）背景自動套用。\n"
	u8"　- 主體更新出現橘色按鈕，一鍵下載、替換、重啟。\n"
	u8"　- 下載以 SHA-256 驗證；失敗自動完整還原。\n"
	u8"新增：pob-zh.exe 內建版本資源，啟動器顯示版本號。\n"
	u8"翻譯：依 3.29 遊戲資料補充字典（物品／天賦／介面）。\n"
	u8"\n"
	u8"v0.2.0（2026-07-23）\n"
	u8"新增：輿圖天賦樹雙賽季並存（3.28+3.29），\n"
	u8"　「版本比較」逐節點、逐詞條比對兩季差異。\n"
	u8"新增：軍團珠寶天賦樹更新至 3.29，\n"
	u8"　圖集隨程式打包，新賽季自動檢查與一鍵更新。\n"
	u8"修正：輿圖／被動樹多項內部自檢問題。\n"
	u8"\n"
	u8"v0.1.0（2026-07-22）\n"
	u8"首次公開發布：零污染繁中化啟動器、翻譯編輯器、\n"
	u8"過濾器編輯器、圖譜配點器、軍團珠寶計算器。\n";
