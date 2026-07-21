// PobTools loot-filter editor: a native ImGui window (same GLFW/ImGui skeleton as
// the translation editor) that opens an existing POE1 .filter, lists its rule
// blocks, and edits colours / font size / alert sound in place, then saves back.
#pragma once

#include <string>

// Blocking GLFW + ImGui loop; fully creates and destroys its window/context so
// the launcher can call it again. `game` is currently POE1-only (reserved).
// `locale` (e.g. "zh-rTW") selects the dictionaries used for Chinese item names.
void ShowFilterEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale);
