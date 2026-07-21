# NOTICE — 第三方元件與資料出處

本專案(**PobTools**,Path of Building 繁體中文化)為 Path of Exile 的**非官方粉絲工具**。
Path of Exile 及其所有遊戲內容、名稱、素材之著作權屬 **Grinding Gear Games**。
本專案與 Grinding Gear Games 無隸屬關係,亦未經其背書。

專案自身原創程式碼以 **MIT** 授權(見 [LICENSE](LICENSE))。以下為所併入 /
依賴 / 取材的第三方元件與其授權,發佈時應一併保留本檔。

---

## 1. 引擎與應用程式碼

| 元件 | 授權 | 著作權 / 出處 |
|---|---|---|
| SimpleGraphic(本專案 fork 的底層引擎) | MIT | © 2016 David Gowor — 完整彙整見 `pob-zh-engine/LICENSE` |
| Path of Building Community(**不隨本專案發佈**,使用者自備) | MIT | © 2016 David Gowor;含一個 LGPL 元件 `base64.lua` |
| Dear ImGui(啟動器 / 編輯器 UI) | MIT | © Omar Cornut |
| nlohmann/json 3.11.3 | MIT | © Niels Lohmann |
| base64(`engine/common/base64.c`,來自 curl) | curl/MIT | © Daniel Stenberg 等 |

> Path of Building Community 本體(`Path of Building.exe`、`Modules/`、`TreeData/` 等)
> **不包含**在本專案的發佈包中,由使用者自行取得並置於 `pob-zh.exe` 旁。

## 2. 翻譯 / 遊戲資料

| 項目 | 授權 / 條款 | 說明 |
|---|---|---|
| Path of Exile 遊戲資料(翻譯字典之取材來源) | GGG 著作權 | 翻譯字典取材自官方客戶端資料,僅供粉絲工具使用,發佈時保留出處聲明,非以 MIT 授權釋出。 |

## 3. 字型

| 檔案 | 授權 | 說明 |
|---|---|---|
| `Fonts/NotoSansTC-Regular.ttf`(**預設** CJK 顯示字型) | **SIL OFL 1.1** | 由 Google/Adobe 思源黑體衍生的 Noto Sans TC,自官方變數字型固定成 Regular(wght 400)靜態 TTF。可自由隨附散布。 |

> 使用者可在啟動器底部的「字型」下拉切換任一放在 `Fonts\` 的 `.ttf`。

---

## 摘要

- **程式碼**:MIT(本專案原創)+ MIT(SimpleGraphic / ImGui / json / curl base64)。
- **POB 本體**:MIT,但**不隨附**,使用者自備。
- **翻譯 / 遊戲資料**:取材自 GGG 版權內容,以粉絲工具用途提供,非以 MIT 授權釋出。
- **字型**:預設 Noto Sans TC(SIL OFL 1.1,可自由隨附散布)。
