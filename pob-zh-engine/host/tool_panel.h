// One tool's content, independent of where it is drawn.
//
// The window / GL context / ImGui context / font atlas / main loop belong to a host; the tool only
// implements this interface. Two hosts:
//   * RunToolWindow (tool_window.cpp) -- the standalone window.
//   * the launcher's tab body -- draws the panel straight into its own frame.
// The per-frame code is written ONCE and both hosts call it.
#pragma once

#include "ui_theme.h"

#include <string>

struct ImFont;

// What the host lends the panel (owned by the host, outliving the panel; read every frame).
// Fonts are read through this EVERY FRAME and never cached: the launcher rebuilds its glyph atlas
// on font change and every earlier ImFont* dangles.
struct ToolPanelHost {
	std::wstring exeDir;
	std::wstring game;      // "poe1" / "poe2"; empty when the tool does not care
	std::wstring locale;
	float scale = 1.0f;

	// The container's HWND, for Win32 dialogs to be owned by. Use this, never GetActiveWindow().
	void* hostHwnd = nullptr;

	// True when drawn as a launcher tab (for genuine differences like closing, never layout).
	bool embedded = false;

	ImFont* body = nullptr;   // CJK-capable; what panels draw with
	// kBigFontGlyphs only (digits, '/', "+-.,~", "c" / "d" suffixes) at ~1.6x body, for big counters
	// and KPI figures. Deliberately not a full face: a second CJK face costs more atlas than the rest of the launcher.
	ImFont* big = nullptr;
	bool cjkOk = false;       // false = the font could not supply Chinese glyphs
};

// The characters ToolPanelHost::big carries; both hosts build it from this.
inline constexpr const char kBigFontGlyphs[] = "0123456789 /+-.,~cd";

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

	// Why Init() said no, in the user's language; empty for "no comment". Reported, not shown:
	// Init runs INSIDE a launcher frame and a modal box there stops the loop that keeps docked
	// POB windows glued to the client area. The host shows it when that is safe.
	virtual const char* InitError() const { return ""; }

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

	// The one place a panel may block. Called after the host rendered and (launcher) after docked
	// POB windows were hidden, so a modal dialog cannot open underneath one.
	virtual void RunDeferred() {}

	// Ask to close. The first call raises the intent; the answer arrives through
	// CloseState() over the following frames, because a panel with unsaved work
	// answers by drawing a prompt.
	virtual ToolCloseState RequestClose() = 0;
	virtual ToolCloseState CloseState() const = 0;

	// Take the request back, returning the panel to Open. Needed because closing the launcher asks
	// EVERY panel at once: if one answers Cancelled, the ones that already agreed must not disappear.
	virtual void AbortClose() = 0;

	// Release threads and GL textures. Must run while the GL context is still
	// current, so the host does this before tearing anything down.
	virtual void Shutdown() {}

	virtual PobUi::Density Density() const { return PobUi::Density::Compact; }

	// Stable across reorderings and runs: it is the ImGui ID scope the panel is drawn in.
	virtual const char* PanelId() const = 0;
};
