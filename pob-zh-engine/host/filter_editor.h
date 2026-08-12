// PobTools loot-filter editor: opens an existing POE1 .filter, lists its rule
// blocks, and edits colours / font size / alert sound in place, then saves back.
//
// The content is an IToolPanel (see tool_panel.h), so the same per-frame code
// serves both a window of its own and a tab inside the launcher.
#pragma once

#include <string>

class IToolPanel;

// Its own window: blocking, creates and destroys everything, so the launcher can
// call it again. `game` is currently POE1-only (reserved). `locale` (e.g.
// "zh-rTW") selects the dictionaries used for Chinese item names.
void ShowFilterEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale);

// As a tab. Caller owns the panel and must Init() it before drawing.
IToolPanel* CreateFilterEditorPanel();
