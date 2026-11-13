// Drawing while the window is being dragged to a new size.
//
// Dragging a border runs the system's modal size loop inside DefWindowProc, i.e.
// inside glfwPollEvents / glfwWaitEventsTimeout, which does not return until the
// mouse is let go; the layout would only catch up on release.
//
// WM_PAINT keeps arriving during that loop (GLFW's window-refresh callback), so
// the launcher and the tool window hand this module their frame function and it
// calls it from that callback -- but only when it is safe to:
//
//   * only inside a move/size loop (GUI_INMOVESIZE). Any other refresh just
//     marks the frame for the main loop, as before (frame_pacing.h);
//   * never while the main loop is itself in a frame or its deferred work. A
//     file dialog, message box or clipboard call made from there pumps
//     messages too, and a WM_PAINT delivered inside it must not start a second
//     frame on top of the half-built one;
//   * never inside a live frame already running.
//
// The frame function is called with live = true and must then skip anything
// that changes what exists (closing, reaping, switching, dialogs, deferred
// work) and leave it to the main loop's next pass. It still runs the frame
// pacer, so the main loop does not present the same picture again.
//
// Only the refresh callback draws. The framebuffer-size callback that comes
// with every step of the drag just marks the frame dirty; drawing from both
// would present each step twice.
#pragma once

#include <functional>

struct GLFWwindow;

namespace LiveResize {

// The re-entrancy rule, without a window: --frame-pacing-selftest checks it.
class Gate {
public:
	// The main loop's own frame and deferred work.
	void EnterMain() { ++main_; }
	void LeaveMain() { if (main_ > 0) --main_; }

	// From the refresh callback: may a live frame start now?
	bool BeginLive(bool inSizeMoveLoop)
	{
		if (!inSizeMoveLoop || main_ > 0 || live_) return false;
		live_ = true;
		return true;
	}
	void EndLive() { live_ = false; ++liveFrames_; }

	bool InMain() const { return main_ > 0; }
	bool InLive() const { return live_; }
	unsigned LiveFrames() const { return liveFrames_; }

private:
	int main_ = 0;
	bool live_ = false;
	unsigned liveFrames_ = 0;
};

// Is the calling thread inside the system move/size loop right now?
bool InSizeMoveLoop();

// Installs the refresh and framebuffer-size callbacks on `win`: both set
// *redraw (the frame-pacing "present even if unchanged" flag); the refresh
// callback also runs `drawLive` when the Gate allows it. Call before the ImGui
// GLFW backend is initialised (it chains the callbacks it installs and leaves
// these two alone). One window per process at a time.
void Install(GLFWwindow* win, bool* redraw);
// The frame function; set once the loop's state exists, cleared (nullptr)
// before that state goes away.
void SetDraw(std::function<void()> drawLive);
// Removes the callbacks and the frame function.
void Uninstall(GLFWwindow* win);

// The gate of the installed window: the main loop brackets its frame with it.
Gate& MainGate();

// Brackets one pass of the main loop.
struct MainScope {
	MainScope() { MainGate().EnterMain(); }
	~MainScope() { MainGate().LeaveMain(); }
	MainScope(const MainScope&) = delete;
	MainScope& operator=(const MainScope&) = delete;
};

} // namespace LiveResize
