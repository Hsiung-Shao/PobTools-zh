# PobTools — Path of Building 繁體中文化啟動器

以本機遊戲檔為翻譯來源的 **Path of Building(POB)繁體中文化工具**。用「零污染」方式
在 POB 外層注入繁中翻譯與介面,**不修改你原本的 POB**,POB 自我更新也不會壞掉。

> 非官方粉絲工具,與 Grinding Gear Games 無關。程式碼採 MIT 授權。

---

## 這是什麼

Path of Exile 台服用語的翻譯資料一直缺乏穩定來源(社群資料落後版本、官方 API 會斷線)。
PobTools 以官方客戶端資料為準,產出約 3.8 萬組英文→繁中對照,內建進一個
**engine-in-exe** 的啟動器裡。

因為翻譯內建在引擎、POB 本體保持完全純淨,**POB 怎麼更新都照跑**——這正是本專案要解決的
「POB 一更新中文化就掛」問題。

### 主要功能

| 功能              | 說明                                                             |
| ----------------- | ---------------------------------------------------------------- |
| 繁中化 POB 啟動器 | POE1 / POE2 皆支援;介面語言可切繁中  / English                  |
| 翻譯編輯器        | 內建字典編輯器,可即時修改、補充翻譯                              |
| 過濾器編輯器      | NeverSink tier-list 式物品過濾器編輯,含中文顯示與圖示            |
| 輿圖策略        | 地圖天賦樹規劃,支援多方案、匯出/分享碼、**新賽季自動更新** |
| 可切換字型        | 預設 Noto Sans TC(OFL),可即時換任何`.ttf`                      |

功能操作細節見 **[docs/USAGE.md](docs/USAGE.md)**。

---

## 下載安裝(一般使用者)

1. 到 [Releases](../../releases) 下載 **`PobTools-<版本>.zip`**,解壓到任一資料夾。
2. 把你的 **Path of Building Community**(POB 本體,自行安裝)放到 `pob-zh.exe` 同一層——
   資料夾名稱不限,內含 `Launch.lua` 即可(名稱含 `PoE2` 視為 PoE2 版)。
3. 雙擊 **`pob-zh.exe`**,選遊戲版本後啟動,POB 即以繁體中文開啟。

> PobTools **不包含** POB 本體(著作權/所有權因素),請自行從
> [https://pathofbuilding.community/](https://pathofbuilding.community/) 取得。

完整圖解步驟與常見問題見 **[docs/INSTALL.md](docs/INSTALL.md)**。

### 更新翻譯

新賽季或翻譯更新時,到 [Releases](../../releases) 下載
**`PobTools-Translations-<版本>.zip`**,把裡面的 `Data` 資料夾覆蓋到安裝目錄即可,
不必重載整包。圖譜天賦樹可在程式內用工具列按鈕線上更新。

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

### 防毒軟體誤判

本程式**未購買程式碼簽章憑證**,加上具備「自動更新(下載並替換自身)」「動態載入模組」
「修改 POB 腳本以進行中文化」等行為特徵,可能被 Windows Defender 等防毒軟體以機器學習
啟發式規則誤判(例如 `Trojan:Win32/Bearfoos.B!ml`,`!ml` 後綴即代表 AI 推測而非特徵碼比對)。

**v0.9.0 起**的 Release 附有 **`SHA256SUMS-<版本>.txt`**,可確認你下載到的檔案與發佈版位元組完全相同:

```powershell
# PowerShell
Get-FileHash .\PobTools-<版本>.zip -Algorithm SHA256
# cmd
certutil -hashfile PobTools-<版本>.zip SHA256
```

> 相同的 hash 代表檔案**未被第三方竄改**,不代表通過任何安全性檢驗。
> 本專案原始碼公開於 GitHub,歡迎自行檢視與編譯。

---

## 社群與回饋

有問題、建議或想許願新功能,歡迎加入 **[Discord 社群](https://discord.gg/6VamPQb8nC)** 討論。

---

## 贊助

此程式已在 GitHub 上開源,歡迎分享給親朋好友使用,也歡迎贊助一杯咖啡 ☕

- **[Buy me a coffee](https://buymeacoffee.com/hsiung)**

---

## 資料出處與授權

專案自身原創程式碼以 **MIT** 授權(見 [LICENSE](LICENSE))。遊戲資料著作權屬
**Grinding Gear Games**;本專案的翻譯資料集僅供粉絲工具使用,發布時保留出處聲明。
併入 / 依賴的第三方元件與授權見 **[NOTICE.md](NOTICE.md)**。
