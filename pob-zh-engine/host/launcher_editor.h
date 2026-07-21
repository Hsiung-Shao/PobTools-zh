// Translation editor window: a separate, resizable ImGui window for viewing,
// editing and gap-scanning the dictionary JSON files. Opened from the launcher
// and torn down completely on close, like ShowLauncher.
#pragma once

#include <string>

// Blocking GLFW+ImGui loop. game/locale select the initial dictionary set
// (the user can switch both inside the editor).
void ShowEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale);
