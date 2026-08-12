// Geometry and window plumbing for docking POB against a container window.
//
// Two halves on purpose: the geometry is pure arithmetic with no Win32 in it, so
// --window-layout-selftest can check it headlessly; the Win32 half is a thin
// wrapper that does what the geometry decided.
#pragma once

#include <string>

namespace WindowMgr {

// Rectangle in screen coordinates. Separate from RECT so the geometry half stays
// free of windows.h (and of the far/near/min/max macro minefield with it).
struct WinRect {
	int x = 0, y = 0, w = 0, h = 0;

	bool operator==(const WinRect& o) const {
		return x == o.x && y == o.y && w == o.w && h == o.h;
	}
};

// Where a docked window belongs: the container's client area minus the tab strip
// along its top. `originX/originY` is the screen position of the container's
// client (0,0) -- docked windows stay top-level, so they are positioned in screen
// coordinates, not in the container's client space.
//
// Never returns a negative height: a container dragged shorter than its own tab
// strip would otherwise produce an inverted rectangle, and SetWindowPos does
// something arbitrary with those.
WinRect ComputeDockRect(int originX, int originY, int clientW, int clientH, int stripH);

// ---- what the dock should do this frame ---------------------------------------
//
// Pulled out of Dock::Update as plain arithmetic for one reason: the minimise
// behaviour was wrong for a long time and nothing could test it. Every Win32 call
// the dock makes is now the consequence of one of these, so the rules can be
// checked headlessly and the Win32 half is left with no decisions of its own.

// What to do with one docked window this frame.
enum class TabAction {
	Hide,      // not the active tab, or the container is minimised
	Show,      // should be on screen but is hidden or minimised
	Position,  // already on screen: keep it glued to the container
};

// Inputs for one tab. `hidden` folds "not visible" and "minimised" together --
// both mean the same thing to the decision, and keeping them apart only invited
// callers to test the wrong one.
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

// A tab minimised on its own (its taskbar button was clicked) takes the container
// with it, so the group behaves as one window. The grace period exists because
// ShowWindowAsync is asynchronous: for a moment after a switch the newly active
// window has not caught up, and reading that transient state as an intentional
// minimise pulled the whole container down.
HostDecision DecideHost(const HostDecisionIn& in);

// Exactly one taskbar button between the container and its docked windows.
// Hiding a docked window also removes ITS button, so dropping the container's
// once and for all left the taskbar empty whenever a normal tab was selected --
// and, worse, whenever the container was minimised, which made the app
// unreachable.
bool ShouldShowOwnTaskbarButton(bool hostMinimised, bool haveActiveTab);

// The style bits a docked window must lose. Kept here, next to the rules that
// depend on them, because forgetting one is silent: the window merely behaves
// like something it no longer looks like.
//
// WS_MAXIMIZE is in this list and used to be missing. It is NOT WS_MAXIMIZEBOX
// (a different bit); POB starts maximised, so the docked window kept a style
// that told Windows it was still maximised while being sized by hand.
unsigned long long DockedStyleMask();

// ---- Win32 half --------------------------------------------------------------

// Ask a window to close, exactly as clicking its X does: POB gets to run its
// "save your build?" prompt. Never TerminateProcess -- that throws away work.
bool RequestClose(void* hwnd);

std::wstring WindowTitle(void* hwnd);

// What a tab should say, given the window's caption.
//
// POB builds its caption as `<build name> (<class>) - Path of Building`
// (Modules/Main.lua SetWindowTitleSubtext), and drops the prefix entirely when no
// build is loaded. So the app name is stripped and, if nothing is left, the tab
// keeps the name it was created with -- "PoE1" beats a blank tab, and beats
// repeating "Path of Building" on every tab.
//
// Returns an empty string to mean "no opinion, leave the tab alone", which is
// what an unreadable caption produces. That is deliberately different from
// returning the fallback: it lets the caller avoid rewriting the label every
// half second for a window it cannot read.
std::wstring ShortenWindowTitle(const std::wstring& caption, const std::wstring& fallback);

// Headless checks for the geometry half; report at <exeDir>PobTools\, 0 = pass.
int RunWindowLayoutSelfTest(const std::wstring& exeDir);

} // namespace WindowMgr
