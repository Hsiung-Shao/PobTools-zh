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
| pure_lua_SHA(`engine/lua/sha2.lua`,POB 執行期備援模組) | MIT | © Egor Skriptunoff — https://github.com/Egor-Skriptunoff/pure_lua_SHA |
| WebView2 SDK(`engine/WebView2Loader.dll` + 標頭;vcpkg `webview2` 1.0.3800.47) | BSD-3-Clause | © Microsoft Corporation。新介面視窗以它嵌入 Edge WebView2;**Evergreen Runtime 本身不隨附**,缺少時啟動器不顯示新介面按鈕並提示下載。 |
| `engine/d3dcompiler_47.dll`(Direct3D HLSL 編譯器;repo 內來源 `pob-zh-engine/host/data/redist/`) | **Microsoft 可再散布元件** | © Microsoft Corporation。隨 Windows SDK 提供、明訂可再散布(檔案描述即 "Direct3D HLSL Compiler for Redistribution",FileVersion 10.0.10150.0);Path of Building Community 亦在自己的 exe 旁隨附同一顆,本專案收錄的就是那份。ANGLE 依名字載入它來編譯譯出的 HLSL;放在 `engine\` 是因為引擎不在 POB 目錄下,少了它 Wine/CrossOver 上的使用者會完全無法啟動 POB。 |

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
| `Fonts/NotoSansKR-Regular.ttf`(韓文 ko-KR 顯示用後援字型) | **SIL OFL 1.1** | Google Noto Sans KR,自官方變數字型 `NotoSansKR[wght].ttf`(google/fonts 儲存庫 `ofl/notosanskr/`,Version 2.004)以 fontTools instancer 固定成 Regular(wght 400)靜態 TTF。預設字型 Noto Sans TC 沒有任何韓文字形,引擎與啟動器在主字型缺字時改用這一顆。可自由隨附散布,授權全文見該儲存庫的 `OFL.txt`。 |
| `Fonts/FZ_ZY.ttf`(可選的替代 CJK 字型) | **商業字型,授權未取得** | 方正系列字型,著作權屬方正集團。本專案**沒有**取得散布授權,隨附純屬歷史沿革(v0.1.0 起就在包裡);程式碼的 MIT 授權**不涵蓋這個檔案**。若權利人要求,將自出貨包移除。使用者可自行刪除 `Fonts\FZ_ZY.ttf`,不影響其他功能(預設字型是上面那一顆)。 |

> 使用者可在啟動器底部的「字型」下拉切換任一放在 `Fonts\` 的 `.ttf`。

### 圖示

| 檔案 | 授權 | 說明 |
|---|---|---|
| `pob-zh-engine/host/data/icons_lucide.inc`(編進 `pob-zh.exe`,不是獨立檔案) | **ISC**(部分圖示另有 MIT) | Lucide Icons 1.47.0(npm `lucide-static`)的 `lucide.ttf`,只保留啟動器用到的 38 個圖示。Copyright (c) 2026 Lucide Icons and Contributors。其中衍生自 Feather 的圖示(如 check、chevron-down、x、info、search、link、download、external-link)另以 MIT 授權,Copyright (c) 2013-present Cole Bemis。兩份授權全文與 Lucide 原始 `LICENSE` 相同,保留於該 `.inc` 檔頭與下方。 |

<details><summary>Lucide ISC License / Feather MIT License 全文</summary>

```
ISC License

Copyright (c) 2026 Lucide Icons and Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

The MIT License (MIT) (for the icons derived from Feather)

Copyright (c) 2013-present Cole Bemis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

</details>

---

## 摘要

- **程式碼**:MIT(本專案原創)+ MIT(SimpleGraphic / ImGui / json / curl base64)+ BSD-3(WebView2 SDK loader)。
- **圖示**:Lucide 子集字型(ISC,部分衍生自 Feather 的圖示為 MIT),編進 `pob-zh.exe`。
- **POB 本體**:MIT,但**不隨附**,使用者自備。
- **翻譯 / 遊戲資料**:取材自 GGG 版權內容,以粉絲工具用途提供,非以 MIT 授權釋出。
- **字型**:預設 Noto Sans TC(SIL OFL 1.1,可自由隨附散布);韓文後援 Noto Sans KR(同為 SIL OFL 1.1)。
  ⚠ 另隨附一顆 `Fonts/FZ_ZY.ttf`(方正系列),**授權未取得**、不在 MIT 涵蓋範圍內,
  詳見上表;它只是可選的替代字型,刪掉不影響任何功能。
