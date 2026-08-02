// ImGui dark launcher window shown before POB starts.
#pragma once

#include "launcher_config.h"

class AppUpdater;

enum class LauncherResult {
	Launch,            // user pressed the launch button; cfg holds the final choices
	OpenEditor,        // user pressed the translation-editor button; cfg holds current choices
	// (filter editor / atlas planner / timeless jewel spawn as child processes
	// inside the launcher loop — no result values; the window stays open)
	ApplyAppUpdate,    // updater staged a new version; caller must swap + relaunch
	Quit,              // window closed (or UI could not be created)
};

// Runs a blocking GLFW+ImGui loop. Fully creates and destroys the window,
// GL context and ImGui context, so it can be called again after POB exits.
// appUpd (optional) drives the update prompt in the header; when its stage
// is ready the window closes and ApplyAppUpdate is returned.
LauncherResult ShowLauncher(LauncherConfig& cfg, const InstallInfo& installs, const std::wstring& exeDir,
                            AppUpdater* appUpd = nullptr);

// Headless check that EVERY shipped font can draw EVERY character the launcher
// puts on screen. ImGui silently substitutes '?' for a glyph the font lacks, so
// a missing character is invisible until someone screenshots it -- which is how
// the version-history bullet ("・", U+30FB) shipped unreadable on FZ_ZY.ttf
// while looking fine on the default Noto Sans TC. Returns 0 when every font
// covers every character. Lives here because the text sources are all in scope.
int RunFontCoverageSelftest(const std::wstring& exeDir);
