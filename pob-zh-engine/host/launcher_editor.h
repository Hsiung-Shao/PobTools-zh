// Translation editor: view, edit and gap-scan the dictionary JSON files.
//
// The content is an IToolPanel (see tool_panel.h), so the same per-frame code
// serves both a window of its own and a tab inside the launcher.
#pragma once

#include <string>

class IToolPanel;

// Its own window: blocking, torn down completely on close, like ShowLauncher.
// game/locale select the initial dictionary set (the user can switch both inside
// the editor).
void ShowEditor(const std::wstring& exeDir, const std::wstring& game, const std::wstring& locale);

// As a tab. Caller owns the panel and must Init() it before drawing.
//
// The concrete type also exposes TakeSavedFlag(): this editor can be editing the
// LAUNCHER's own dictionary, so a save means the strings the launcher is drawing
// with are now stale. The launcher checks it and reloads.
IToolPanel* CreateTranslationEditorPanel();
bool TranslationEditorPanelSaved(IToolPanel* panel);
