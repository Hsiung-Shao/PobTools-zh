// The standalone-window host for an IToolPanel.
//
// This is the boilerplate every ShowXxx() used to repeat: create a window and a GL
// context, build a CJK font atlas, run a frame loop, translate the window's close
// button into a close request, tear it all down. Written once here so the separate
// and tabbed modes cannot drift apart -- the only thing that differs between them
// is this file versus the launcher's tab body, and neither contains any of the
// tool's own drawing code.
#pragma once

#include <cstdint>
#include <string>

class IToolPanel;

// A standalone tool window follows the launcher's font-size setting while it is
// open. The launcher is another process (or this one, before it handed over to
// the tool), so the only channel is pob-zh.ini: the window looks at the file's
// write time once a second -- never per frame -- re-reads the ini only when that
// moved, and rebuilds its atlas and style only when FontSize itself changed.
//
// Pure, so --frame-pacing-selftest can drive it on a fake clock.
namespace ToolZoom {

constexpr double kPollInterval = 1.0;   // seconds between looks at the ini's write time

class Watch {
public:
	// What the window was built with, and the ini's write stamp at that moment.
	void Start(double now, std::uint64_t stamp, int fontSize)
	{
		last_ = now;
		stamp_ = stamp;
		fontSize_ = fontSize;
	}
	// Time to look at the write stamp again? A clock that went backwards counts.
	bool Due(double now) const { return now - last_ >= kPollInterval || now < last_; }
	// The stamp just read. True = the ini was written since the last commit:
	// re-read it, then Commit. Restarts the interval either way.
	bool StampChanged(double now, std::uint64_t stamp)
	{
		last_ = now;
		return stamp != stamp_;
	}
	// The re-read result, under the stamp it was read at. Only call this when
	// the stamp was the same before and after the read (a launcher save is a
	// run of separate key writes; a read in the middle of one is not committed,
	// so the next poll tries again). True = FontSize changed: rebuild.
	bool Commit(std::uint64_t stamp, int fontSize)
	{
		stamp_ = stamp;
		if (fontSize == fontSize_) return false;
		fontSize_ = fontSize;
		return true;
	}
	int FontSize() const { return fontSize_; }

private:
	double last_ = 0.0;
	std::uint64_t stamp_ = 0;
	int fontSize_ = 0;
};

} // namespace ToolZoom

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
