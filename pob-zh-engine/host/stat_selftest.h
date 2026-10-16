// --stat-selftest：translation_lookup_stat(依 stats_by_id.json 翻單行 stat)的契約。
// 詳見 stat_selftest.cpp 開頭。不寫出貨字典:在 %TEMP% 建字典複本並以
// POB_ZH_DATADIR 指過去,結束時刪除。
#pragma once

#include <string>

// statsJson:stats_by_id.json 的路徑;空字串 = 出貨字典 Data\poe1\zh-rTW\ 裡那份。
// 0 = 全過,3 = 有失敗,2 = 測試環境準備失敗。
int RunStatSelftest(const std::wstring& statsJson);
