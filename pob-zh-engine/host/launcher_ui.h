// ImGui dark launcher window shown before POB starts.
#pragma once

#include "launcher_config.h"

enum class LauncherResult {
	Launch,            // user pressed the launch button; cfg holds the final choices
	OpenEditor,        // user pressed the translation-editor button; cfg holds current choices
	OpenFilterEditor,  // user pressed the loot-filter-editor button
	OpenAtlasPlanner,  // user pressed the atlas-tree-planner button
	OpenTimelessJewel, // user pressed the timeless-jewel calculator button
	Quit,              // window closed (or UI could not be created)
};

// Runs a blocking GLFW+ImGui loop. Fully creates and destroys the window,
// GL context and ImGui context, so it can be called again after POB exits.
LauncherResult ShowLauncher(LauncherConfig& cfg, const InstallInfo& installs, const std::wstring& exeDir);
