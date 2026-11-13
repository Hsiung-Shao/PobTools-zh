// How fast the POB window is allowed to draw.
//
// Upstream SimpleGraphic's only brake on its frame loop is the swap waiting for vsync, and it does not hold:
//   * THE PASSIVE TREE NEVER STOPS: every frame differs, nothing is elided, and the swap did not block (0.07 ms),
//     so the loop ran as fast as Lua allowed (~120 fps).
//   * A STATIC PAGE STILL PRESENTS: the build list elided every frame yet blitted and presented 144 times a
//     second (37% of an integrated GPU at 1440p).
// Capping the rate bounds the total whatever makes a frame expensive on somebody's machine.
//
// Two rules, both pure functions below so --perf-selftest can check them:
//   1. A frame budget. Foreground and background (unfocused but still drawing) each have a fps cap, 0 = no cap;
//      the loop sleeps off what is left of the budget.
//   2. An elided frame is not presented, unless the picture must refresh anyway (size changed, a screenshot, ImGui
//      content) or a second has passed since the last present. An unpresented frame did not wait for vsync either,
//      so with no cap it is paced at the monitor refresh -- never a busy spin.
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

// Sleeps with sub-millisecond precision (high-resolution waitable timer, Windows 10 1803+), else with a 1 ms timer
// period. Plain Sleep() would round a 16.6 ms budget up to the 15.6 ms tick and turn a 60 fps cap into 32.
void PreciseSleep(double seconds);

} // namespace PobFrameCap

// --perf-selftest: frame cap decisions and the performance log. 0 = pass.
int RunPerfSelfTest(const std::wstring& exeDir);
