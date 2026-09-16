// The new interface: a plain Win32 window hosting WebView2 (--modern-ui).
//
// The page (dist\ui\, a Svelte bundle) is the whole UI; this window is a
// forwarder. It starts a headless POB child (pob-zh.exe --engine-headless, see
// headless_proc.h), hands every line the page posts to the child's stdin, and
// posts every line the child writes back to the page. The only requests it
// answers itself are the few "host.*" methods that need Win32 (open a folder,
// set the title, close) -- everything about the build goes through the bridge.
//
// Runs in its own process, started from the launcher like the other tools:
// WebView2 wants an STA message pump of its own, and the launcher's loop is
// GLFW + ImGui. Even in the launcher's tabbed mode this stays a separate
// window (the dock only adopts GLFW windows).
#pragma once

#include <string>

struct LauncherConfig;

// True when the new interface can be opened here: WebView2 Runtime present,
// the loader DLL beside the engine, the built page at <exeDir>ui\index.html.
// `why` (optional) receives the first missing piece, for a message or a log.
bool ModernUiAvailable(const std::wstring& exeDir, std::wstring* why);

// Blocks until the window closes. Returns the process exit code. `openBuild`
// (optional) is a build .xml the page loads as soon as the engine is up --
// what a relaunch after a POB self-update, or a shell association, would pass.
int ShowModernUi(const std::wstring& exeDir, const std::wstring& game,
                 const std::wstring& locale, const LauncherConfig& cfg,
                 const std::wstring& openBuild = std::wstring());
