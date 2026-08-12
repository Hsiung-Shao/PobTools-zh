// PobTools atlas passive-tree planner (PoE1).
#pragma once

#include <string>

class IToolPanel;

// As a tab. Caller owns the panel and must Init() it before drawing.
IToolPanel* CreateAtlasPlannerPanel();

// As a window of its own; blocking, returns when the user closes it.
void ShowAtlasPlanner(const std::wstring& exeDir, const std::wstring& locale);
