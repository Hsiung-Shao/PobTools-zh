// PobTools atlas passive-tree planner window (PoE1). Blocking GLFW+ImGui loop
// like the filter editor; returns when the user closes the window.
#pragma once

#include <string>

void ShowAtlasPlanner(const std::wstring& exeDir, const std::wstring& locale);
