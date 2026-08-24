// 搜尋字串產生器 — the regex tool, as a standalone window and as a launcher tab.
//
// Same two entry points every other tool has: CreateRegexToolPanel() for the
// tabbed launcher, ShowRegexTool() for the separate window that "--regex" starts.
#pragma once

#include <string>

class IToolPanel;

IToolPanel* CreateRegexToolPanel();

void ShowRegexTool(const std::wstring& exeDir, const std::wstring& game,
                   const std::wstring& locale);
