// The standalone-window host for an IToolPanel.
//
// This is the boilerplate every ShowXxx() used to repeat: create a window and a GL
// context, build a CJK font atlas, run a frame loop, translate the window's close
// button into a close request, tear it all down. Written once here so the separate
// and tabbed modes cannot drift apart -- the only thing that differs between them
// is this file versus the launcher's tab body, and neither contains any of the
// tool's own drawing code.
#pragma once

#include <string>

class IToolPanel;

struct ToolWindowDesc {
	const char* titleUtf8 = "PobTools";
	int defW = 1180, defH = 740;
	// Keep the window inside the monitor's work area rather than centring it at its
	// preferred size. The translation editor wants this; it is big enough that the
	// taskbar matters.
	bool clampToWorkArea = false;
};

// Runs until the panel says it is closed. 0 = normal exit.
int RunToolWindow(IToolPanel& panel, const ToolWindowDesc& desc,
                  const std::wstring& exeDir, const std::wstring& game,
                  const std::wstring& locale);
