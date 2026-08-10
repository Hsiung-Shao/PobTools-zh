// 多佔位符詞條的數值填位檢查。詳見 placeholder_selftest.cpp 開頭。
// 全部走 Data\ 裡真正出貨的字典,唯讀,不寫任何字典檔。
#pragma once

#include <string>

// --placeholder-selftest：邊角案例表 + 出貨字典端到端 + 突變測試。0 = 全過。
int RunPlaceholderSelftest();

// --placeholder-dump <out.tsv> [poe1|poe2] [legacy]：把每一條含數值格的詞條各跑
// 一次正向與反向,輸出可逐行比對的快照。
//
// `legacy` 讓這一次跑在舊的「照 '#' 出現順序填」行為下,所以 A/B 的兩邊是同一顆
// 二進位、同一份字典、同一批探針,唯一的變因就是填位規則本身 —— 拿兩次建構的產物
// 對比,任何一點編譯或資料差異都會混進來當雜訊。
int RunPlaceholderDump(const std::wstring& outPath, const std::string& game, bool legacy);

// --placeholder-probe <english>：單筆正向查詢,回報實際輸出(排查用)。
int RunPlaceholderProbe(const std::wstring& text);
