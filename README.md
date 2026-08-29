# PobTools — Path of Building 繁體中文化啟動器

以本機遊戲檔為翻譯來源的 **Path of Building(POB)繁體中文化工具**。用「零污染」方式
在 POB 外層注入繁中翻譯與介面,**不修改你原本的 POB**,POB 自我更新也不會壞掉。

> 非官方粉絲工具,與 Grinding Gear Games 無關。程式碼採 MIT 授權。

💬 有問題、建議或想許願新功能,歡迎加入 **[Discord 社群](https://discord.gg/6VamPQb8nC)** 討論。

---

## 這是什麼

Path of Exile 台服用語的翻譯資料一直缺乏穩定來源(社群資料落後版本、官方 API 會斷線)。
PobTools 以官方客戶端資料為準,產出約 10 萬組英文→中文對照,內建進一個
**engine-in-exe** 的啟動器裡。

因為翻譯內建在引擎、POB 本體保持完全純淨,**POB 怎麼更新都照跑**——這正是本專案要解決的
「POB 一更新中文化就掛」問題。

### 主要功能

| 功能              | 說明                                                             |
| ----------------- | ---------------------------------------------------------------- |
| 中文化 POB 啟動器 | POE1 / POE2 皆支援;介面語言可切繁中 / 简中 / English            |
| 翻譯編輯器        | 內建字典編輯器,可即時修改、補充翻譯                              |
| 過濾器編輯器      | NeverSink tier-list 式物品過濾器編輯,含中文顯示與圖示            |
| 輿圖策略          | 地圖天賦樹規劃,支援多方案、匯出/分享碼、**新賽季自動更新**      |
| 軍團珠寶計算器    | 永恆軍團珠寶種子計算,支援中文搜尋與交易連結                      |
| 搜尋字串產生器    | 勾選詞綴產生遊戲內搜尋用的正規表示式,POE1 / POE2 清單皆支援      |
| 可切換字型        | 預設 Noto Sans TC(OFL),可即時換任何`.ttf`                      |

功能操作細節見 **[docs/USAGE.md](docs/USAGE.md)**。

---

## 下載安裝

1. 到 [Releases](../../releases) 下載 **`PobTools-<版本>.zip`** 解壓。
2. 另外準備 **Path of Building Community** 本體(PobTools 不包含 POB):
   [原版 POB 最新版下載](https://github.com/PathOfBuildingCommunity/PathOfBuilding/releases/latest)。
3. 完整步驟、資料夾擺放方式、翻譯資料更新與常見問題,見 **[docs/INSTALL.md](docs/INSTALL.md)**。

---

## 免責聲明

**使用本程式所產生的一切後果與風險,由使用者自行承擔。**

### 它做了什麼、沒做什麼

- **不會修改你磁碟上的 POB 檔案**。翻譯與介面都在啟動時注入,POB 本體保持純淨,
  POB 自我更新照常運作。
- **會在記憶體中攔截 POB 的文字繪製與貼上處理**,把英文換成繁中;
  也會在載入時修改 POB 的部分 Lua 腳本——**僅存在於記憶體,不寫回你的 POB 檔案**。
- **不會接觸 Path of Exile 遊戲本身**:不注入遊戲行程、不讀寫遊戲記憶體、
  不修改遊戲檔案。本程式只與 POB 這個第三方計算器互動。
- 會在自己的資料夾與 POB 資料夾寫入設定、翻譯字典與快取。
- 串接的 API(官方交易站、GitHub、poecdn 圖示)皆為官方公開資源,
  **請勿於短時間內大量查詢,以免造成伺服器負擔**。

### 與官方的關係

This product isn't affiliated with or endorsed by Grinding Gear Games or Garena in any way.

本程式為**非官方粉絲工具**,與 Grinding Gear Games、Garena 無任何隸屬關係,亦未經其背書。
Path of Exile 及其所有遊戲內容之著作權屬 Grinding Gear Games。

### 防毒軟體誤判與更新驗證

- 本程式**未購買 Windows 程式碼簽章憑證**,加上「自動更新、動態載入模組、記憶體修補
  POB 腳本」等行為特徵,可能被防毒軟體以啟發式規則誤判(`!ml` 後綴即 AI 推測型偵測)。
  原始碼公開於 GitHub,歡迎自行檢視與編譯。
- **手動下載**的人可用 Release 附的 `SHA256SUMS-<版本>.txt` 核對雜湊
  (`certutil -hashfile <檔名> SHA256`),確認檔案與發佈版位元組相同。
- **自動更新不需要你做任何事**:v0.26.0 起,程式主體與翻譯資料在安裝前都會以
  **編譯進執行檔的公鑰**驗證發佈清單的 ECDSA P-256 簽章
  (公鑰:[`pob-zh-engine/host/update_pubkeys.h`](pob-zh-engine/host/update_pubkeys.h)),
  簽章缺少或驗不過**一律拒絕安裝**,絕不會「驗不了就照裝」。
- 注意:這是「更新內容是不是維護者發的」的來源驗證,**不是** Windows 程式碼簽章,
  無法消除上述防毒誤判。

---

## 資料出處與授權

專案自身原創程式碼以 **MIT** 授權(見 [LICENSE](LICENSE))。遊戲資料著作權屬
**Grinding Gear Games**;本專案的翻譯資料集僅供粉絲工具使用,發布時保留出處聲明。
併入 / 依賴的第三方元件與授權見 **[NOTICE.md](NOTICE.md)**。
