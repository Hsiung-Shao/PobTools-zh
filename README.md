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
| 圖譜配點器        | 地圖天賦樹規劃,支援多方案、匯出/分享碼、**新賽季自動更新** |
| 可切換字型        | 預設 Noto Sans TC(OFL),可即時換任何`.ttf`                      |

功能操作細節見 **[docs/USAGE.md](docs/USAGE.md)**。

---

## 下載安裝(一般使用者)

1. 到 [Releases](../../releases) 下載 **`PobTools-<版本>.zip`**,解壓到任一資料夾。
2. 把你的 **Path of Building Community**(POB 本體,自行安裝)放到 `pob-zh.exe` 同一層,
   資料夾命名為 `PathOfBuildingCommunity`(POE1)/ `PathOfBuildingCommunity-PoE2-Portable`(POE2)。
3. 雙擊 **`pob-zh.exe`**,選遊戲版本後啟動,POB 即以繁體中文開啟。

> PobTools **不包含** POB 本體(著作權/所有權因素),請自行從
> [https://pathofbuilding.community/](https://pathofbuilding.community/) 取得。

完整圖解步驟與常見問題見 **[docs/INSTALL.md](docs/INSTALL.md)**。

### 更新翻譯

新賽季或翻譯更新時,到 [Releases](../../releases) 下載
**`PobTools-Translations-<版本>.zip`**,把裡面的 `Data` 資料夾覆蓋到安裝目錄即可,
不必重載整包。圖譜天賦樹可在程式內用工具列按鈕線上更新。

---

## 資料出處與授權

專案自身原創程式碼以 **MIT** 授權(見 [LICENSE](LICENSE))。遊戲資料著作權屬
**Grinding Gear Games**;本專案的翻譯資料集僅供粉絲工具使用,發布時保留出處聲明。
併入 / 依賴的第三方元件與授權見 **[NOTICE.md](NOTICE.md)**。
