// Geometry and window plumbing for docking POB against a container window.
//
// Two halves: pure geometry (--window-layout-selftest checks it headlessly) and a thin Win32 wrapper that applies it.
#pragma once

#include <string>

namespace WindowMgr {

// Rectangle in screen coordinates (not RECT, to keep the geometry half free of windows.h macros).
struct WinRect {
	int x = 0, y = 0, w = 0, h = 0;

	bool operator==(const WinRect& o) const {
		return x == o.x && y == o.y && w == o.w && h == o.h;
	}
};

// Where a docked window belongs: the container's client area minus the tab strip. `originX/originY`
// is the screen position of the container's client (0,0) (docked windows stay top-level).
// Never returns a negative height (SetWindowPos does something arbitrary with inverted rectangles).
WinRect ComputeDockRect(int originX, int originY, int clientW, int clientH, int stripH);

// what the dock should do this frame
//
// Plain arithmetic pulled out of Dock::Update so the rules are testable headlessly; every Win32
// call the dock makes is a consequence of one of these.

// What to do with one docked window this frame.
enum class TabAction {
	Hide,      // not the active tab, or the container is minimised
	Show,      // should be on screen but is hidden or minimised
	Position,  // already on screen: keep it glued to the container
};

// Inputs for one tab. `hidden` folds "not visible" and "minimised" together.
struct TabState {
	bool isActive = false;
	bool hidden = false;
};

TabAction DecideTab(bool hostMinimised, const TabState& t);

// Container-level decisions, evaluated a few times a second rather than per frame.
struct HostDecisionIn {
	bool hostMinimised = false;
	bool haveActiveTab = false;   // active index is within range
	bool activeTabMinimised = false;
	unsigned long long msSinceTabSwitch = 0;
};
struct HostDecision {
	bool minimiseContainer = false;  // the active tab went away: follow it down
	bool sinkContainer = false;      // keep the container below the active window
};

// A tab minimised on its own takes the container with it. The grace period exists because
// ShowWindowAsync is asynchronous: right after a switch the newly active window hasn't caught up,
// and reading that as an intentional minimise pulled the whole container down.
HostDecision DecideHost(const HostDecisionIn& in);

// Exactly one taskbar button between the container and its docked windows (hiding a docked window
// removes ITS button, so dropping the container's for good would leave the app unreachable when minimised).
bool ShouldShowOwnTaskbarButton(bool hostMinimised, bool haveActiveTab);

// The style bits a docked window must lose (forgetting one is silent). Note WS_MAXIMIZE is NOT
// WS_MAXIMIZEBOX; POB starts maximised, so a kept WS_MAXIMIZE tells Windows it is still maximised.
unsigned long long DockedStyleMask();

// Win32 half

// Ask a window to close, exactly as clicking its X does: POB gets to run its
// "save your build?" prompt. Never TerminateProcess -- that throws away work.
bool RequestClose(void* hwnd);

std::wstring WindowTitle(void* hwnd);

// What a tab should say, given the window's caption. POB builds it as
// `<build name> (<class>) - Path of Building` (Modules/Main.lua SetWindowTitleSubtext) and drops
// the prefix when no build is loaded: the app name is stripped, and if nothing is left the tab
// keeps the name it was created with.
// Returns "" for "no opinion, leave the tab alone" (an unreadable caption) -- deliberately not the
// fallback, so the caller doesn't rewrite the label every half second.
std::wstring ShortenWindowTitle(const std::wstring& caption, const std::wstring& fallback);

// The tab strip's label for a docked window: what it says, plus what ImGui is to
// identify it BY.
//
// The separator has to be "###", not "##": only "###" resets the hash (ImHashStr), otherwise the
// visible text is part of the id and a renamed build changes the tab's identity (nothing selected,
// Dock::Update hides the window in use). The pid, not the tab index, so closing an earlier tab
// doesn't hand its widget state to the next one.
std::string DockTabLabel(const std::string& utf8Label, unsigned long pid);

// Headless checks for the geometry half; report at <exeDir>PobTools\, 0 = pass.
int RunWindowLayoutSelfTest(const std::wstring& exeDir);

} // namespace WindowMgr
