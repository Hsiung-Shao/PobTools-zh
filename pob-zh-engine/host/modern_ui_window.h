// The new interface: a plain Win32 window hosting WebView2 (--modern-ui).
//
// The page (dist\ui\, a Svelte bundle) is the whole UI; this window is a forwarder: it starts a headless POB child
// (pob-zh.exe --engine-headless, see headless_proc.h), passes each page line to the child's stdin and each child line
// back to the page. It answers only the few "host.*" methods needing Win32 (open a folder, set the title, close).
// Runs in its own process, like the other tools: WebView2 wants its own STA message pump, and the launcher's loop is
// GLFW + ImGui. Even in the launcher's tabbed mode this stays a separate window (the dock only adopts GLFW windows).
#pragma once

#include <string>

struct LauncherConfig;

// True when the new interface can be opened here: WebView2 Runtime present,
// the loader DLL beside the engine, the built page at <exeDir>ui\index.html.
// `why` (optional) receives the first missing piece, for a message or a log.
bool ModernUiAvailable(const std::wstring& exeDir, std::wstring* why);

// (Where WebView2 cannot run, ShowModernUi opens the same page in the system
// browser instead -- modern_ui_browser.h -- so "usable" below counts that too.)

// ModernUiAvailable plus the remembered bridge gate for this POB (same
// install, same version, same bridge.lua): what the launcher's "Launch"
// button asks before it opens the new interface instead of the classic one.
bool ModernUiUsableFor(const std::wstring& exeDir, const std::wstring& pobDir, const std::string& pobVersion);

// Blocks until the window closes. Returns the process exit code. `openBuild`
// (optional) is a build .xml the page loads as soon as the engine is up --
// what a relaunch after a POB self-update, or a shell association, would pass.
// `importUri` (optional) is a validated "Open in PoB" link (pob:// / pob2://,
// see pob_protocol.h): the page downloads and imports it once the engine is
// up, and a bridge-gate fallback hands it to the classic window instead.
int ShowModernUi(const std::wstring& exeDir, const std::wstring& game,
                 const std::wstring& locale, const LauncherConfig& cfg,
                 const std::wstring& openBuild = std::wstring(),
                 const std::wstring& importUri = std::wstring());
