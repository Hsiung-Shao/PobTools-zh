// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// System Video Header
//

#include <string>

// =======
// Classes
// =======

// Video settings flags
enum vidFlags_e {
	VID_RESIZABLE = 0x04,
	VID_MAXIMIZE = 0x08,
	VID_USESAVED = 0x10,
};

// Saved video state structure
struct sys_vidSave_s {
	int		size[2] = {};
	int		pos[2] = {};
	bool	maximised = false;
	int		fbSize[2] = {};
	float	dpiScale = 1.0f;
};

// Video settings structure
struct sys_vidSet_s {
	bool	shown = false;		// Show window?
	int		flags = 0;		// Flags
	int		mode[2] = {};	// Window size
	int		minSize[2] = {};	// Minimum size for resizable windows
	sys_vidSave_s save; // Saved state
};

// Diagnostic trace for the window opacity feature. No-op unless the environment
// variable POB_ZH_OPACITY_TRACE names a file; then each call appends one line.
// Windows only (defined in win/sys_video.cpp).
void sys_opacity_trace(const char* fmt, ...);
bool sys_opacity_trace_enabled();

// ==========
// Interfaces
// ==========

// System Video
class sys_IVideo {
public:
	static sys_IVideo* GetHandle(class sys_IMain* sysHnd);
	static void FreeHandle(sys_IVideo* hnd);

	sys_vidSave_s vid;	// Current state

	// Window opacity in percent, 100 = opaque (feature off). Windows only: set
	// from POB_ZH_WINDOW_OPACITY at window creation and live by the pob-zh
	// launcher (a WM_APP message, see sys_video.cpp). The renderer draws POB's
	// chrome fills (side bar, top bar, tree bottom toolbar) with this alpha so
	// the layer below them shows through; the window itself stays opaque.
	// Exposed to Lua as PobToolsWindowOpacity() so the injected script can
	// extend the passive tree under the chrome while it is active.
	int windowOpacityPct = 100;
	// Background image for the whole POB window (absolute path, UTF-8; empty =
	// POB's own striped background) and its brightness in percent, plus the
	// liquid-glass blur strength for the chrome panels (0 = plain fill). Same
	// plumbing as windowOpacityPct: environment at startup, window messages
	// live (sys_video.cpp); read from Lua via PobToolsBackground().
	std::string bgPath;
	int bgBrightPct = 50;
	int glassBlurPct = 0;
	// Opacity of the passive tree's own tiled backdrop (the layer under the
	// nodes), percent; 100 = upstream behaviour, lower lets the background
	// image show through it, 0 = hidden.
	int treeBgPct = 100;
	// Frames the UI loop must still render because one of the values above
	// just changed (set by the message handlers in sys_video.cpp, consumed by
	// ui_main_c::Frame). Upstream POB stops rendering entirely while the
	// window has no focus and the cursor is elsewhere -- exactly the state
	// POB is in while the player drags a slider in the launcher -- so without
	// this the new look only appeared once the cursor came back (2026-09-12).
	int appearanceRedrawFrames = 0;

	virtual	int		Apply(sys_vidSet_s* set) = 0;	// Apply settings

	virtual void	SetForeground() = 0; // Activate the window if shown
	virtual bool	IsActive() = 0; // Get activated status
	virtual void	FramebufferSizeChanged(int width, int height) = 0; // Respond to framebuffer size change
	virtual void	SizeChanged(int width, int height, bool max) = 0; // Respond to window size change
	virtual void	PosChanged(int x, int y) = 0; // Respond to window position change
	virtual void	GetMinSize(int &width, int &height) = 0; // Get minimum window size
	virtual void	SetVisible(bool vis) = 0;		// Show/hide window
	virtual bool	IsVisible() = 0; // Get whether the window is shown
	virtual void	SetTitle(const char* title) = 0;// Change window title
	virtual void*	GetWindowHandle() = 0;			// Get window handle
	virtual void	GetRelativeCursor(int &x, int &y) = 0; // Get cursor position relative to window
	virtual void	SetRelativeCursor(int x, int y) = 0; // Set cursor position relative to window
	virtual bool	IsCursorOverWindow() = 0; // Get whether the cursor is over the window, including obstructions
};
