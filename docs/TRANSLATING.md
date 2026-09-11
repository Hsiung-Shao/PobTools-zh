# 為 PobTools 增加或維護一種語言

這一頁寫給翻譯者:語言怎麼打包、檔案長什麼樣、怎麼在本機測試、怎麼送回來。
不需要編譯器,一個文字編輯器加 PobTools 本身就夠。

English version: [TRANSLATING.en.md](TRANSLATING.en.md)

## 語言是怎麼被找到的

PobTools **掃磁碟**找語言,沒有任何清單要登記。一種語言就是三個資料夾(各對應一個「槽」),
每個裡面放一份 `meta.json`:

```
Data\
  poe1\<locale>\      PoE1 版 Path of Building 執行時用的字典
  poe2\<locale>\      PoE2 版 Path of Building 執行時用的字典
  launcher\<locale>\  啟動器自己的文字(按鈕、設定、訊息)
```

`<locale>` 是資料夾名,例如 `zh-rTW`、`zh-rCN`、`ko-KR`、`ja-JP`。把資料夾放好、重開啟動器,
語言就出現在視窗底部的下拉。只有一個遊戲有的語言會標「(僅 PoE1)」/「(僅 PoE2)」。
`en` 永遠存在、沒有資料夾——查不到字典的東西一律顯示英文原文。

目前出貨:`zh-rTW`(繁體中文,基準集)、`zh-rCN`(簡體中文)、`ko-KR`(韓文)。

### `meta.json`

```json
{
  "version": "1.0.0",
  "locale": "ko-KR",
  "display_name": "한국어",
  "source": "poe1",
  "load_order": ["tags.json", "items.json", "gems.json", "ui.json",
                 "stats.json", "passives.json", "uniques.json", "monsters.json"],
  "incomplete_translation_whitelist": ["DPS", "PoB", "DoT", "AoE"],
  "glossary_blacklist": ["UNUSED"]
}
```

| 欄位 | 意思 |
|---|---|
| `display_name` | 語言下拉顯示的名字。可省略,省略時顯示資料夾名。 |
| `load_order` | 字典檔清單,**照載入順序**。沒列在這裡的檔不會被載入。 |
| `incomplete_translation_whitelist` | 允許留在譯文裡的英文縮寫(DPS、AoE…)。 |
| `glossary_blacklist` | 內建編輯器的詞彙表永遠不提供的鍵。 |

同一語系的所有字典會合併成**一張查找表,後載入的蓋掉先載入的**。所以 `tags.json`
(「Fire」「Cold」這種詞綴片段)排最前,權威檔(`ui.json`、`stats.json`、`gems.json`…)排後面:
片段絕不能蓋掉完整名稱。除非你清楚為什麼,否則沿用基準集的 `load_order`。

啟動器槽只有一個檔:`load_order: ["launcher.json"]`。

## 字典檔

每個字典都是一個帶 `entries` 的 JSON 物件:

```json
{
  "source_files": ["Items_Armour.csv", "Items_Weapons.csv"],
  "is_base_items": true,
  "entries": {
    "Chaos Orb": "카오스 오브",
    "{0}% increased maximum Life": "최대 생명력 {0}% 증가",
    "Adds {0} to {1} Cold Damage": "냉기 피해 {0}~{1} 추가"
  }
}
```

- **鍵 = Path of Building 畫出來的英文原文,逐字相同**——拼字、標點、空格、換行(JSON 字串裡的 `\n`)
  都一樣。PobTools 是拿畫出來的字串逐字去查,差一個字元就永遠查不到。
- `source_files` 與 `is_base_items` 是從基準集抄來的記錄欄位,原樣保留。`is_base_items: true`
  標記「這一檔的條目是物品基底」(把你語言寫的物品貼回 PoB 時用得到)。
- 缺鍵只是顯示英文。**寧可留空,不要猜**——錯的翻譯比一行英文糟。

### 佔位符與色碼——原樣保留

| 鍵裡有 | 值裡要有 | 規則 |
|---|---|---|
| `{0}`、`{1}`、`{0:+d}` | 同一組佔位符 | 每個索引都要出現,順序依你的語言調整。索引決定哪個數字放哪裡,絕不重新編號。 |
| `#` | `#` | 詞綴模板裡單獨的 `#` 是數字格。PoB 自己的設定標籤(`# of Poison on enemy:`)裡它是「數量」,可以譯成字。 |
| `^7`、`^xRRGGBB` | 同樣的色碼、同樣的數量 | PobTools 會按色碼分段各自翻譯,所以鍵裡很少有色碼;有的話每一個都要留。 |
| `[Term|Display text]` | `[Term|你的譯文]` | 遊戲檔標記:`|` 前面是連結目標必須保持英文,只翻後面。 |

從只有 `#` 的來源(例如交易站 API)回填 `{0}` 樣式的值,**只有恰好一個佔位符時才安全**;兩個以上,
你的語言的語序決定哪個數字是哪個,由左到右回填會不聲不響地對調。這種行請改從遊戲檔取,那裡的索引本來就對。

### 文字從哪裡來——以及「不猜」這條規則

PobTools 對每一種語言都用同一條規則:**官方遊戲文字優先;官方沒有的就留英文並列進清單**。
權威順序:

1. **遊戲客戶端自己的檔案**(GGPK)裡你的語言——物品、寶石、天賦、傳奇、怪物、詞綴,
   逐字等於遊戲印出來的。
2. **官方交易站的資料 API**(`https://<realm>.pathofexile.com/api/trade/data/stats`,
   以 `id` 對接英文站)。用字與遊戲相同,當第二證人。
3. **社群翻譯**(例如舊的粉絲工具)——只用在遊戲裡根本不存在的文字:Path of Building
   自己的分頁、計算欄位、設定選項。這大約占每本字典的一半,也是翻譯者真正要做的部分。

社群來源與官方不一致時,採官方值,分歧寫進衝突清單。請你自己改的時候也這樣做:遊戲已有的詞就用遊戲的詞。

`ko-KR` 就是在 2026 年 9 月照這個流程建的:基準鍵約 82% 有譯文(遊戲內容來自遊戲檔與交易站 API,
PoB 自身介面來自 PoeCharm3 社群檔案),其餘顯示英文。韓文的啟動器文字是**機器草稿,尚未經母語者校對**
——`Data\launcher\ko-KR\launcher.json` 是韓文翻譯者最先能幫上忙的地方。

## 啟動器自己的文字(`Data\launcher\<locale>\launcher.json`)

```json
{
  "entries": {
    "Launch": "실행",
    "Settings": "설정",
    "Check for updates": "업데이트 확인"
  }
}
```

鍵是編譯進啟動器的英文標籤;漏掉的鍵自動回到英文,所以檔案不完整也沒關係。要拿完整鍵清單當新語言的範本:

```
pob-zh.exe --launcher-strings-export ko-KR
```

會寫出 `Data\launcher\ko-KR\launcher.json`,每個鍵都填上英文原文(既有檔案是**合併不是覆蓋**:
你的值優先、缺的鍵補上、程式已經不認得的鍵留在最後)。對 `zh-rTW` 同一個指令填的是編譯進去的中文。

文字必須是出貨字型畫得出來的(見下節)。啟動器只在啟動時用所有已安裝語言的文字建一次字形圖集,
沒有任何出貨字型有的字會畫成 `?`。

## 字型

預設字型是 Noto Sans TC。`Fonts\` 裡其他每一顆 `.ttf` 都會被啟動器與 POB 視窗當**後援字形**用,
所以 Noto Sans TC 沒有的文字系統要另外放一顆字型進 `Fonts\`。目前出貨:Noto Sans TC(中文、拉丁、假名)、
Noto Sans KR(韓文)、FZ_ZY(可選的中文替代)。新字型的條件:

- TrueType 外框(帶 `glyf` 表的 `.ttf`)。OpenType/CFF 在 POB 視窗畫得出來,但啟動器讀不了。
- 想隨版出貨就要有允許再散布的授權(Google 的 Noto 系列是 SIL OFL 1.1,首選;變數字型要先固定成靜態 Regular)。
- 出貨前要在 `NOTICE.md` 申報——打包腳本會擋下未申報的字型。

只在自己機器上用就不用管這些:任何 `.ttf` 放進 `Fonts\`,在啟動器底部的字型清單選它即可。

## 本機測試

1. 資料夾放進 `Data\`、重開啟動器、選語言。
2. **設定**頁把「自動更新翻譯資料」設為**否**,否則下一次資料更新會蓋掉你的檔。(或者把每個槽指到安裝目錄
   **以外**的資料夾——同一頁的「翻譯資料」段;更新永遠碰不到外部資料夾。)
3. 開 POB 看。內建的**翻譯編輯器**可以即時改目前選用語言的字典。
4. 無 GUI 的檢查,全部用結束碼回報(0 = 通過),在安裝目錄執行:

   ```
   pob-zh.exe --tr "Chaos Orb" ko-KR          # 引擎對這一個鍵回什麼
   pob-zh.exe --font-coverage-selftest        # 字典裡每個字都有某顆出貨字型畫得出來
   pob-zh.exe --launcher-strings-selftest     # launcher.json 的形狀、回英文的機制
   pob-zh.exe --launcher-config-selftest      # 磁碟上的語言偵測
   ```

   `--tr` 把報告寫到標準輸出;從看不到輸出的 shell 執行時導向到檔案。`pob-zh.exe` 是 GUI 子系統程式,
   判斷要看 `$LASTEXITCODE` / `%ERRORLEVEL%`,不要看主控台文字。

## 送回來

對 [PobTools-zh](https://github.com/Hsiung-Shao/PobTools-zh) 的 `pob-zh-engine/dist/Data/<slot>/<locale>/`
開 pull request。說明裡請寫:

- 哪個語系、哪些檔,是新語言還是更新;
- **每一部分的來源**(遊戲檔 / 交易站 API / 自己翻的)——維護者審查會把每個改動的鍵對回官方遊戲文字,
  事先知道來源會快很多;
- 啟動器文字:你是不是母語者。

翻譯資料走自己的發佈線(`data-<n>`),與程式分開,合併後不必等程式更新就能到使用者手上。
需要新字型的新語言要等下一個程式版。

## 已知限制

- Path of Building 自己的介面(分頁、計算欄位、設定選項)在任何語言都沒有官方翻譯,那一半是社群工作。
- 內建編輯器與 `--tr` 預設 `zh-rTW`;要驗其他語言請照上面明確帶 locale。
- `item_metadata.json` 與 `synonyms.json`(貼上物品的解析、搜尋同義詞)只有 `zh-rTW` 有;
  其他語言貼上的物品只認英文。
