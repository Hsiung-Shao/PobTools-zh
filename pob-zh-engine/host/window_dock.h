// Docking other processes' windows into a container window, WITHOUT making them
// child windows.
//
// Why not child windows: SetParent across processes attaches the two threads'
// input queues, which makes every cross-process window call synchronous
// (SWP_ASYNCWINDOWPOS stops working) and deadlocks the container on the first
// resize. Setting the container as OWNER instead is no better -- it crashes
// GLFW's message handling on the very next poll. Both measured 2026-08-05; see
// agent-data error_setparent_cross_process_embed_deadlock.md.
//
// So docked windows stay top-level. They are stripped of their frame and kept
// glued to the container's client area, and the container sinks itself below the
// active one to keep the z-order right. The engine is not modified for any of
// this: everything is done from outside, to windows POB opened normally.
#pragma once

#include <string>
#include <vector>

namespace WindowDock {

// A window this container is managing. `pid` is known as soon as the process is
// spawned; `hwnd` appears some time later, once it has created its window.
struct Tab {
	unsigned long pid = 0;
	void*         hwnd = nullptr;     // HWND, null until the window shows up
	std::wstring  label;              // what the tab strip shows
	bool          docked = false;     // frame stripped and being positioned
};

// Manages every docked window for one container.
//
// Non-copyable: it holds window state that must not be duplicated, and the
// callbacks below capture a pointer to it.
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

	// Called from the container's GLFW move/size callbacks. Dragging a window
	// puts Windows into a modal message loop during which glfwPollEvents never
	// returns, so without these the docked window is left behind for the whole
	// drag.
	void OnHostMoved();
	void OnHostFocused();

	// Ask a tab to close, exactly as clicking its X would: POB gets to run its
	// "save your build?" prompt.
	void RequestClose(size_t index);

	// Put every docked window back the way it was found -- frame, geometry and
	// taskbar button. Safe to call more than once.
	void RestoreAll();

	const std::vector<Tab>& Tabs() const { return tabs_; }
	bool Empty() const { return tabs_.empty(); }

private:
	void Adopt(Tab& t);
	void Position(Tab& t, bool force, bool sync = false);
	void FixZOrder(Tab& t);

	struct Original { long long style = 0, exStyle = 0; int x = 0, y = 0, w = 0, h = 0; };

	void*             host_ = nullptr;
	std::vector<Tab>  tabs_;
	std::vector<Original> orig_;      // index-aligned with tabs_
	int               stripH_ = 0;
	int               active_ = -1;
	bool              droppedOwnButton_ = false;
	unsigned long long lastFind_ = 0, lastKeep_ = 0, lastSwitch_ = 0;
};

// Feasibility run, opened with --dock-spike. Kept because it is the smallest
// thing that exercises the whole path end to end.
int RunDockSpike(const std::wstring& exeDir, bool spawnPob = true, bool verbose = false);

} // namespace WindowDock
