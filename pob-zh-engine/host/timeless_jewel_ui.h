// Timeless Jewel calculator window (Phase 3): a Vilsol-style search form in
// Traditional Chinese. Standalone GLFW+ImGui window, like the atlas planner.
#pragma once

#include <string>

// Blocking window; fully creates and tears down its own GLFW/ImGui/GL context.
void ShowTimelessJewel(const std::wstring& exeDir, const std::wstring& locale);

// Debug: render one frame of the passive-tree canvas to pt_render.bmp next to
// the exe, headless ("--pt-render [zoom cx cy]"; zoom<=0 = auto-fit).
int RunPassiveTreeRender(const std::wstring& exeDir, float zoom, float cx, float cy);
