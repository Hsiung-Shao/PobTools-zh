// 王戰收益 — boss kill profit table (PoE1), as a launcher tab and a standalone window.
#pragma once

#include <string>

class IToolPanel;

IToolPanel* CreateBossProfitPanel();
// "pob-zh.exe --boss-profit"
void ShowBossProfitTool(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale);
// "pob-zh.exe --boss-profit-selftest": headless checks of boss_profit.h plus the shipped data file
// (exeDir\Data\boss_drops_poe1.json). No network. Returns 0 when every check passes.
int RunBossProfitSelftest(const std::wstring& exeDir);
