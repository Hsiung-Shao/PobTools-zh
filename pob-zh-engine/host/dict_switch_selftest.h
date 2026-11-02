// 啟動器「可停用的字典檔」(kOptionalDicts)從設定到引擎載入的檢查。
// 字典放 %TEMP% 的拋棄式資料夾,不碰出貨字典與 pob-zh.ini。
#pragma once

#include <string>

// --dict-switch-selftest:ini 來回、env 字串、引擎開/關載入、出貨 meta.json。0 = 全過。
int RunDictSwitchSelfTest(const std::wstring& exeDir);
