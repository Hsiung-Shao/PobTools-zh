// One tool's content, independent of where it is drawn.
//
// Each tool used to be a function that owned a window, a GL context, an ImGui
// context, a font atlas and a main loop. That works when the tool IS the
// application, but it cannot be a tab in the launcher: the launcher already has
// all of those, and there can only be one of each.
//
// So a tool is split in two. Everything about being an application moves to a
// host; what is left is this interface. Two hosts implement it:
//
//   * RunToolWindow (tool_window.cpp) -- the standalone window, i.e. what the
//     separate-window mode has always done. It is the old ShowXxx() function with
//     the tool's content lifted out.
//   * the launcher's tab body -- draws the panel straight into its own frame.
//
// The per-frame code is written ONCE and both hosts call it, which is the point:
// two copies of "draw the filter editor" would drift, and only one of them would
// be the one anybody tested.
#pragma once

#include "ui_theme.h"

#include <string>

struct ImFont;

// What the host lends the panel. Owned by the host and outliving the panel; the
// panel keeps a pointer and reads through it every frame.
//
// In particular the fonts are read through this EVERY FRAME and never cached: the
// launcher rebuilds its glyph atlas when the user changes font, and every ImFont*
// from before that is dangling afterwards.
struct ToolPanelHost {
	std::wstring exeDir;
	std::wstring game;      // "poe1" / "poe2"; empty when the tool does not care
	std::wstring locale;
	float scale = 1.0f;

	// The container's HWND, for Win32 dialogs to be owned by. Panels must use this
	// rather than GetActiveWindow(): that happens to return the right window today
	// and would silently stop doing so the moment a panel opened a dialog while
	// something else had focus.
	void* hostHwnd = nullptr;

	// True when drawn as a launcher tab. Panels should need this only for things
	// that are genuinely different (a standalone window can be closed by its own X;
	// a tab cannot), never for layout.
	bool embedded = false;

	ImFont* body = nullptr;   // CJK-capable; what panels draw with
	bool cjkOk = false;       // false = the font could not supply Chinese glyphs
};

// Why a panel is still on screen after being asked to close.
enum class ToolCloseState {
	Open,       // nobody asked
	Asking,     // the panel is showing its own prompt; keep drawing it, keep it visible
	Closed,     // done -- Shutdown() and drop it
	Cancelled,  // the user said no; whoever started the close must abandon it
};

class IToolPanel {
public:
	virtual ~IToolPanel() = default;

	// Load everything. False means the panel cannot run (a data file is missing);
	// the host shows a message instead of the panel rather than an empty tab.
	virtual bool Init(const ToolPanelHost& host) = 0;

	// Exactly one frame of content, inside the window the host has already begun.
	//
	// The contract, all of which the launcher's tab body satisfies naturally:
	//   * no glfw* calls -- the panel does not own a window
	//   * no ImGui::Begin for a root window; there is already one, and the cursor is
	//     at the top-left of the content area (use GetContentRegionAvail)
	//   * no io.DisplaySize -- that is the whole viewport, not this panel's space
	//   * ID / style / font / child stacks balanced on exit
	//   * no blocking. Anything that blocks -- a Win32 dialog, a thread join, a
	//     large file read -- records an intent here and does it in RunDeferred().
	virtual void Frame() = 0;

	// The one place a panel may block. Called after the host has rendered and, in
	// the launcher, after the docked POB windows have been hidden -- so a modal
	// dialog cannot open underneath one of them, and a long pause cannot leave a
	// docked window stranded mid-frame.
	virtual void RunDeferred() {}

	// Ask to close. The first call raises the intent; the answer arrives through
	// CloseState() over the following frames, because a panel with unsaved work
	// answers by drawing a prompt.
	virtual ToolCloseState RequestClose() = 0;
	virtual ToolCloseState CloseState() const = 0;

	// Release threads and GL textures. Must run while the GL context is still
	// current, so the host does this before tearing anything down.
	virtual void Shutdown() {}

	virtual PobUi::Density Density() const { return PobUi::Density::Compact; }

	// Stable across reorderings and across runs: it is the ImGui ID scope the panel
	// is drawn in, so widget state would be lost (or worse, shared) if it moved.
	virtual const char* PanelId() const = 0;
};
