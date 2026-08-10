# PobTools 安裝教學

PobTools 是 **Path of Building（POB）繁體中文化工具**。它用「零污染」方式在
POB 外層注入中文翻譯與介面,**不修改你原本的 POB**。

> 非官方粉絲工具,與 Grinding Gear Games 無關。

---

## 一、你需要準備

1. **Windows**(10 / 11)。
2. **Path of Building Community**(POB 本體)—— PobTools **不包含** POB,請自行安裝:
   - 官方下載:[https://pathofbuilding.community/](https://pathofbuilding.community/)
   - 或你電腦上已安裝好的 POB 資料夾。

---

## 二、安裝步驟

### 1. 下載並解壓 PobTools

下載 **`PobTools-<版本>.zip`**,解壓到任一資料夾(例如 `D:\PobTools\`)。
解壓後你會看到 `pob-zh.exe` 和 `engine`、`Data`、`Fonts` 等資料夾。

### 2. 把 POB 本體放到 pob-zh.exe 旁邊

在 `pob-zh.exe` **同一層**放入 POB 資料夾。**資料夾名稱不限**——只要裡面有
`Launch.lua` 就會被偵測到(`PathOfBuildingCommunity`、
`PathOfBuildingCommunity-Portable` 或自己改過的名稱都可以)。

| 遊戲            | 判別方式                                                |
| --------------- | ------------------------------------------------------- |
| Path of Exile 1 | 任一內含 `Launch.lua` 的資料夾                          |
| Path of Exile 2 | 資料夾名稱需含 `PoE2`(例:`PathOfBuildingCommunity-PoE2-Portable`) |

最終長這樣(資料夾名稱僅為範例):

```
D:\PobTools\
├─ pob-zh.exe
├─ engine\
├─ Data\
├─ Fonts\
├─ PathOfBuildingCommunity\              ← 你的 POE1 POB
└─ PathOfBuildingCommunity-PoE2-Portable\ ← 你的 POE2 POB(可選)
```

> 只玩 POE1 就只放一個;只玩 POE2 就只放名稱含 `PoE2` 的那個。
> 也支援把 `pob-zh.exe` 連同 `engine\`、`Data\`、`Fonts\` 直接放進 POB
> 資料夾裡(`Launch.lua` 與 `pob-zh.exe` 同層),但建議如上圖並排放置。
> 同時有多個符合的資料夾時,優先採用官方名稱,其餘按名稱排序取第一個。

### 3. 啟動

雙擊 **`pob-zh.exe`**,在啟動器選擇遊戲版本後按「啟動」。POB 會以繁體中文開啟。

> 各功能(翻譯編輯器、過濾器編輯器、輿圖策略、字型/語言切換)的操作方式見
> **[USAGE.md](USAGE.md)**。

---

## 三、更新翻譯（不用重載整包）

翻譯資料自成一條發佈線,版號是 `data-1`、`data-2`…,與程式版號無關。
程式**平常會自己更新它**,不必手動處理;要手動換的話,到 Releases 找
標著 `data-<編號>` 的那一則,下載 **`PobTools-Data-<編號>.zip`**,解壓後把裡面的
`Data` 資料夾**覆蓋**到你的 PobTools 安裝資料夾即可。

> 不想被自動覆蓋(例如你自己在改翻譯):設定頁的「自動更新翻譯資料」選「否」。
> 選了否之後有新資料時仍會通知,可以按「立即套用一次」單次取用。

> Releases 頁面上的三個檔:`PobTools-<版本>.zip` 是第一次安裝用的完整包;
> `PobTools-update-<版本>.zip` 是程式自動更新用的,**不含翻譯資料**,手動下載它
> 會得到一個沒有中文的 POB;`PobTools-Data-<編號>.zip` 只有翻譯資料。

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
  → 啟動器底部有語言下拉(繁中 / English)。

---

## 五、授權與支持

程式碼採 MIT 授權;翻譯資料取材自遊戲內容(著作權屬 GGG),僅供粉絲工具使用。
詳見 [LICENSE](../LICENSE) 與 [NOTICE.md](../NOTICE.md)。

若這個工具對你有幫助,歡迎在啟動器的「關於」裡請作者喝杯咖啡 ☕。
