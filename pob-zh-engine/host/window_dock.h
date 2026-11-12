// Docking other processes' windows into a container window, WITHOUT making them child windows.
//
// Why not: SetParent across processes attaches the two threads' input queues (every cross-process
// window call becomes synchronous, SWP_ASYNCWINDOWPOS stops working, the container deadlocks on
// resize); setting the container as OWNER crashes GLFW's message handling on the next poll.
// See agent-data error_setparent_cross_process_embed_deadlock.md.
//
// So docked windows stay top-level: frame stripped, glued to the container's client area, and the
// container sinks itself below the active one. Everything is done from outside; the engine is untouched.
#pragma once

#include <string>
#include <vector>

namespace WindowDock {

// A window this container is managing. `pid` is known at spawn; `hwnd` appears later.
struct Tab {
	unsigned long pid = 0;
	void*         hwnd = nullptr;     // HWND, null until the window shows up
	std::wstring  label;              // what the tab strip shows
	bool          docked = false;     // frame stripped and being positioned

	// POB starts maximized, and a maximized window ignores SetWindowPos's size. Un-maximising is
	// asynchronous, so adoption waits a frame or two before stripping the frame (doing both at once
	// wrote the old style, WS_MAXIMIZE included, back over the restore).
	bool          awaitingRestore = false;

	// The label the tab was created with: fallback when the window title cannot be read.
	std::wstring  baseLabel;

	// Bring this tab to the front once, then clear. A newly started tool otherwise flashes up and is
	// hidden (the dock hides non-active tabs); embedded panels focus themselves on open, so docked ones must too.
	bool          wantFocus = true;
};

// Manages every docked window for one container.
//
// Non-copyable: callbacks below capture a pointer to it.
class Dock {
public:
	Dock() = default;
	Dock(const Dock&) = delete;
	Dock& operator=(const Dock&) = delete;
	~Dock();

	// `host` is the container's HWND. Must be called before anything else.
	void Init(void* host, const std::wstring& logPath);

	// Start tracking a process. The window is adopted when it appears.
	void Track(unsigned long pid, const std::wstring& label);

	// Call once per frame. `stripH` is the height of the tab strip in the
	// container's client area; `activeIndex` is which tab is visible (-1 for
	// none, i.e. a non-window tab such as Settings is showing).
	//
	// Reaps dead tabs, adopts new windows, positions the active one, hides the
	// rest, and keeps the z-order and minimise state in step.
	void Update(int stripH, int activeIndex);

	// Called from the container's GLFW move/size callbacks (dragging puts Windows into a modal loop
	// where glfwPollEvents never returns, so the docked window would be left behind).
	void OnHostMoved();
	void OnHostFocused();

	// Ask a tab to close, exactly as clicking its X would: POB gets to run its
	// "save your build?" prompt.
	void RequestClose(size_t index);

	// Put every docked window back the way it was found -- frame, geometry and
	// taskbar button. Safe to call more than once.
	void RestoreAll();

	const std::vector<Tab>& Tabs() const { return tabs_; }

	// Consume the one-shot focus request for a tab (see Tab::wantFocus).
	bool TakeFocusRequest(size_t index);
	bool Empty() const { return tabs_.empty(); }

private:
	void Adopt(Tab& t);
	// Always asynchronous. There is deliberately no synchronous option: see
	// OnHostMoved, and S18/S19 in the style selftest.
	void Position(Tab& t, bool force);
	void FixZOrder(Tab& t);

	// What the window looked like before the dock touched it. `captured`: adoption may run more than
	// once, and only the FIRST pass sees the untouched window.
	struct Original {
		long long style = 0, exStyle = 0;
		int x = 0, y = 0, w = 0, h = 0;
		bool captured = false;
	};

	void*             host_ = nullptr;
	std::vector<Tab>  tabs_;
	std::vector<Original> orig_;      // index-aligned with tabs_
	int               stripH_ = 0;
	int               active_ = -1;
	bool              droppedOwnButton_ = false;
	unsigned long long lastFind_ = 0, lastKeep_ = 0, lastSwitch_ = 0, lastTitle_ = 0;
	// Rate limit for the move callback (a drag fires it faster than a window can repaint; each is a cross-process request).
	unsigned long long lastDragPos_ = 0;
	// Last logged container minimise state: logged on the EDGE only (a per-frame line would bury the transition).
	int               loggedHostMin_ = -1;
};

// Feasibility run, opened with --dock-spike (smallest end-to-end exercise of the path).
int RunDockSpike(const std::wstring& exeDir, bool spawnPob = true, bool verbose = false);

// Adoption and restoration against a real window this process owns, opened with
// --dock-style-selftest. Report at <exeDir>PobTools\, 0 = pass.
//
// It builds its own MAXIMIZED window rather than driving POB: POB's maximized state depends on the
// user's last run, so a check on it would pass vacuously.
int RunDockStyleSelfTest(const std::wstring& exeDir);

} // namespace WindowDock
