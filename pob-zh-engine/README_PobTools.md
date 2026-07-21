# pob-zh-engine — PobTools(里程碑 1:解決「POB 更新就掛」)

> 原名 PobCharm,已更名為 **PobTools**。內部 INI 區段 `[PoeCharm]`、Lua 全域 `PoeCharm*` 保留為相容別名。

繁體中文化的 Path of Building 啟動器,採 **engine-in-exe + 翻譯內建進引擎** 架構。
這是 `PathOfBuilding-SimpleGraphic`(自家 SimpleGraphic fork)的衍生版,把
`pob-translate-proxy` 的雙向翻譯邏輯直接內建進引擎,**不再需要 lua51 proxy**。

## 為什麼這樣能解決「更新就掛」

- 引擎是**版本無關的 Lua 宿主**:`ui_main.cpp` 把 `argv[0]`(= POB 的 `Launch.lua`)當腳本跑,
  不解析 POB manifest、不做版本檢查。只要 SimpleGraphic 對 Lua 的 API(`DrawString`/`Paste`…)
  契約穩定,**POB 的 Lua/資料怎麼更新都照跑**。
- 翻譯內建在引擎的 `l_*` 函式,**丟掉了會脆弱脫節的 lua51 proxy**(轉發 342 函式 + `lua_setfield` 攔截)。
- 外部 POB 保持**完全純淨**(零污染),可自我更新;字型與字典都放在 pob-zh.exe 旁。

## 相對於原引擎的改動(里程碑 1)

| 檔案 | 改動 |
|---|---|
| `translate/translation_manager.{cpp,h}` | 從 `pob-translate-proxy` 移植(雙向翻譯);`poe1` 路徑改為依 `POB_GAME` |
| `deps/nlohmann/json.hpp` | vendor(translation_manager 依賴) |
| `ui_api.cpp` | `DrawString`/`DrawStringWidth`/`DrawStringCursorIndex` 文字參數經 `tr_display()`(英→中);`Paste` 改用 Win32 讀剪貼簿 + `translation_reverse_text()`(中→英);新增 `PobToolsTranslate`/`PobToolsReverse` Lua 全域(舊名 `PoeCharm*` 保留為別名);`InitAPI` 末呼叫 `translation_init()` |
| `ui_main.cpp` | `ScriptShutdown` 配對 `translation_shutdown()` |
| `engine/render/r_font.cpp` | CJK 字型多一個 `POB_ZH_FONTDIR/Fonts/` 搜尋來源(零污染,字型放 exe 旁) |
| `host/host_main.cpp` | 新增 thin host exe:載入 `SimpleGraphic.dll`、定位外部 POB、設 `POB_*` env、呼叫 `RunLuaFileAsWin` |
| `CMakeLists.txt` | 加 translate 源碼 + include;新增 `pob-zh` host target(`/utf-8`) |

## 建置

```powershell
cmake -B build -S . -G "Visual Studio 18 2026" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```
> `vcpkg/` 是連回 `PathOfBuilding-SimpleGraphic/vcpkg` 的 junction(共用已 bootstrap 的 toolchain)。

部署時 `dist/` 需另外放入:
- `dist/Data/Translate/poe1/{locale}/*.json`(翻譯字典)
- `dist/Data/Translate.json`
- `dist/Fonts/NotoSansTC-Regular.ttf`(CJK 字型)

## 執行

把純淨的 POB 放在 `pob-zh.exe` 旁(`PathOfBuildingCommunity\`),或設 `POB_PATH`:

```powershell
$env:POB_GAME   = "poe1"      # 或 poe2
$env:POB_LOCALE = "zh-rTW"    # zh-rTW / zh-rCN / ko-KR
$env:POB_PATH   = "C:\path\to\PathOfBuildingCommunity"   # 可省略,預設找 exe 旁同名資料夾
.\dist\pob-zh.exe
```
亦可改用 `dist\pob-zh.ini`:
```ini
[PobTools]
Game=poe1
Locale=zh-rTW
```
> 舊的 `[PoeCharm]` 區段仍可讀取(自動遷移),儲存時會寫成 `[PobTools]`。

## 驗證狀態

- ✅ `SimpleGraphic.dll` + `pob-zh.exe` 建置成功(configure + build 皆 exit 0)。
- ✅ Smoke test:`pob-zh.exe` 對真實 POB 2.65.0 啟動,成功建立視窗、執行 POB Lua、未崩潰。
- ⬜ **需人工 GUI 確認**(無頭環境無法看畫面):
  1. 介面顯示繁中(非方框),tooltip 寬度正確。
  2. 遊戲複製中文裝備 → POB 匯入框貼上 → 解析成英文裝備。
  3. 對 `PathOfBuildingCommunity/` 跑 POB 自身 `Update.exe` 後重啟,仍正常 = 問題 1 證明。
