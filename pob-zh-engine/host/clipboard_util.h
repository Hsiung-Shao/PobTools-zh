// Shared Win32 clipboard helpers (UTF-8 <-> CF_UNICODETEXT).
//
// GLFW's clipboard (sys_main_c::ClipboardPaste, and ImGui when it routes
// through the GLFW backend) mangles CJK to '?', so every feature that moves
// item text through the clipboard must use these instead. Extracted from
// timeless_jewel_ui.cpp so the filter editor's item import can share them.
#pragma once

#include <string>

// Read the clipboard as UTF-8 text ("" on failure). `ownerHwnd` may be null.
std::string ReadClipboardUtf8(void* ownerHwnd);

// Put UTF-8 text on the clipboard as CF_UNICODETEXT (what PoB's paste reads).
bool WriteClipboardUtf8(void* ownerHwnd, const std::string& textUtf8);
