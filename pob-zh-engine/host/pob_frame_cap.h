// How fast the POB window is allowed to draw.
//
// Upstream SimpleGraphic has exactly one brake on its frame loop: the swap
// waiting for vsync. Measured on 2026-09-17 that brake does not hold:
//
//   * THE PASSIVE TREE NEVER STOPS. Every frame there differs from the last,
//     so nothing is elided, and the swap did not block at all (0.07 ms per
//     Swap): the loop ran as fast as Lua allowed, ~120 fps on a desktop CPU.
//   * A STATIC PAGE STILL PRESENTS. The build list elided every single frame
//     (0 redraws a second) and still blitted and presented 144 times a second,
//     which alone cost 37% of an integrated GPU at 1440p.
//
// A user report (3060 Ti at 87% GPU for POB, CPU at 0.9%) is what started the
// measurement; its exact cause was never reproduced, which is the point of
// capping the rate: whatever makes one frame expensive on somebody's machine,
// the total is now bounded by how many frames are allowed.
//
// Two rules, both pure functions below so --perf-selftest can check them:
//
//   1. A frame budget. Foreground (window focused) and background (unfocused
//      but still drawing: cursor over it, a coroutine or subscript running)
//      each have a fps cap, 0 = no cap. The frame loop sleeps off whatever is
//      left of the budget after the frame.
//   2. An elided frame is not presented, unless the picture must be refreshed
//      anyway (size changed, a screenshot, ImGui content on screen) or a
//      second has passed since the last present. A frame that was not
//      presented did not wait for vsync either, so with no cap configured it
//      is paced at the monitor refresh instead -- never a busy spin.
#pragma once

#include <string>

namespace PobFrameCap {

constexpr int kDefaultForegroundFps = 60;
constexpr int kDefaultBackgroundFps = 15;
constexpr int kMaxFps = 1000;              // anything above is treated as "no cap"
constexpr double kPresentKeepAlive = 1.0;  // seconds: present at least this often

// Clamp a configured value: negative or absurd -> 0 (no cap).
int ClampFps(int fps);

struct CapInputs {
	double now = 0.0;             // seconds, monotonic
	double frameStart = 0.0;      // when this frame began (same clock)
	bool   active = false;        // window has focus
	int    foregroundFps = kDefaultForegroundFps;
	int    backgroundFps = kDefaultBackgroundFps;
	int    refreshHz = 60;        // monitor refresh, <=0 = unknown (treated as 60)
	bool   presented = true;      // this frame reached the screen (a presented frame waited on vsync)
};

// Seconds the loop should sleep after this frame; 0 = go straight on.
double SleepSeconds(const CapInputs& in);

// The fps limit that applies to a frame (0 = none); exposed for the log.
int EffectiveFps(const CapInputs& in);

struct PresentInputs {
	bool elided = false;          // the frame's draw commands matched the last drawn frame
	bool sizeChanged = false;     // framebuffer size differs from the last present
	bool screenshot = false;      // a screenshot will read the back buffer
	bool imguiContent = false;    // ImGui draw data is not empty (debug windows)
	bool firstFrame = false;
	double now = 0.0;
	double lastPresent = -1.0e9;
};

// Should this frame be blitted and swapped?
bool ShouldPresent(const PresentInputs& in);

// Sleeps with sub-millisecond precision where the OS allows it (a
// high-resolution waitable timer, Windows 10 1803+), otherwise with a 1 ms
// timer period for the duration of the sleep. Plain Sleep() would round a
// 16.6 ms budget up to the 15.6 ms tick and turn a 60 fps cap into 32.
void PreciseSleep(double seconds);

} // namespace PobFrameCap

// --perf-selftest: frame cap decisions and the performance log. 0 = pass.
int RunPerfSelfTest(const std::wstring& exeDir);
