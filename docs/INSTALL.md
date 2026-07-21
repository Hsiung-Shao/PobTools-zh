# PobTools 安裝教學

PobTools 是 **Path of Building（POB）繁體中文化工具**。它用「零污染」方式在
POB 外層注入中文翻譯與介面,**不修改你原本的 POB**。

> 非官方粉絲工具,與 Grinding Gear Games 無關。

---

## 一、你需要準備

1. **Windows**(10 / 11)。
2. **Path of Building Community**(POB 本體)—— PobTools **不包含** POB,請自行安裝:
   - 官方下載:<https://pathofbuilding.community/>
   - 或你電腦上已安裝好的 POB 資料夾。

---

## 二、安裝步驟

### 1. 下載並解壓 PobTools

下載 **`PobTools-<版本>.zip`**,解壓到任一資料夾(例如 `D:\PobTools\`)。
解壓後你會看到 `pob-zh.exe` 和 `engine`、`Data`、`Fonts` 等資料夾。

### 2. 把 POB 本體放到 pob-zh.exe 旁邊

在 `pob-zh.exe` **同一層**放入 POB 資料夾,名稱需正確:

| 遊戲 | 資料夾名稱(放在 pob-zh.exe 旁) |
|---|---|
| Path of Exile 1 | `PathOfBuildingCommunity`(內含 `Launch.lua`) |
| Path of Exile 2 | `PathOfBuildingCommunity-PoE2-Portable` |

最終長這樣:

```
D:\PobTools\
├─ pob-zh.exe
├─ engine\
├─ Data\
├─ Fonts\
├─ PathOfBuildingCommunity\              ← 你的 POE1 POB
└─ PathOfBuildingCommunity-PoE2-Portable\ ← 你的 POE2 POB(可選)
```

> 只玩 POE1 就只放 `PathOfBuildingCommunity`;只玩 POE2 就只放另一個。

### 3. 啟動

雙擊 **`pob-zh.exe`**,在啟動器選擇遊戲版本後按「啟動」。POB 會以繁體中文開啟。

> 各功能(翻譯編輯器、過濾器編輯器、圖譜配點器、字型/語言切換)的操作方式見
> **[USAGE.md](USAGE.md)**。

---

## 三、更新翻譯（不用重載整包）

新賽季或翻譯有更新時,只要下載 **`PobTools-Translations-<版本>.zip`**,
解壓後把裡面的 `Data` 資料夾**覆蓋**到你的 PobTools 安裝資料夾即可,
不必重新下載整個程式。

> 圖譜(地圖天賦樹)可在程式內用工具列的更新按鈕直接線上更新,通常不必手動換檔。

---

## 四、常見問題

- **啟動器說「未偵測到任何 POB」**
  → 檢查 POB 資料夾名稱是否正確(見上表),且和 `pob-zh.exe` 在**同一層**。

- **開啟後是亂碼 / 沒有中文字**
  → 確認 `Fonts\` 資料夾裡有字型檔(預設 `NotoSansTC-Regular.ttf`,解壓時未遺漏)。

- **想換字型**
  → 啟動器底部有「字型」下拉,可即時切換 `Fonts\` 內任何 `.ttf`。
  想加自己的字型,把 `.ttf` 丟進 `Fonts\` 再重開啟動器即可(建議用 TrueType 靜態字型)。

- **防毒軟體攔截**
  → 本工具會啟動 POB 並注入翻譯,可能被誤報;請自行斟酌加入信任。

- **想切換介面語言**
  → 啟動器底部有語言下拉(繁中 / 簡中 / English)。

---

## 五、授權與支持

程式碼採 MIT 授權;翻譯資料取材自遊戲內容(著作權屬 GGG),僅供粉絲工具使用。
詳見 [LICENSE](../LICENSE) 與 [NOTICE.md](../NOTICE.md)。

若這個工具對你有幫助,歡迎在啟動器的「關於」裡請作者喝杯咖啡 ☕。
