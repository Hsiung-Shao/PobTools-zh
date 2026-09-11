// Why the launcher does not simply draw sixty frames a second.
//
// An ImGui window redraws itself from scratch every iteration, and until
// v1.5.0 both the launcher and the tool windows ran that loop flat out:
// glfwPollEvents, build the frame, present, repeat -- paced only by the
// swap waiting for vsync. Two things were wrong with that:
//
//   * MINIMISED, THE SWAP DOES NOT WAIT. There is nothing to present, so the
//     loop spun as fast as the CPU allowed: measured at 124% of one core and
//     23% of the GPU for a launcher sitting in the taskbar behind POB.
//   * ON SCREEN, NOTHING CHANGES MOST OF THE TIME. Sixty presents a second of
//     an identical picture cost 3-4% of a core and a couple of percent of the
//     GPU on a desktop, several times that on an integrated one.
//
// So the loop is paced here instead, on three rules that do not depend on the
// swap chain at all:
//
//   1. Wait for events, with a timeout. Fast cadence (one 60 Hz frame) for a
//      short tail after any input, or while the caller says it is busy (a
//      worker, a timer, a closing sequence); otherwise ten iterations a second.
//      Any input wakes the wait immediately, so the idle cadence is invisible.
//   2. Minimised: no present at all. The ImGui frame still runs so every
//      timer and state machine in the loop keeps its exact behaviour.
//   3. Unchanged draw data is not presented. The frame is hashed after
//      ImGui::Render(); if it equals the last frame that reached the GPU, the
//      previous image simply stays on screen. The engine does the same for the
//      POB window. A refresh or resize event, the first frame, and a return
//      from the minimised state all force a present regardless.
//
// The decisions are a pure table over a handful of inputs so that
// --frame-pacing-selftest can check them without a window.
#pragma once

#include <cstdint>
#include <string>

struct ImDrawData;

namespace FramePacing {

constexpr double kActiveTail = 0.5;        // seconds of fast cadence after the last input
constexpr double kActiveWait = 1.0 / 60.0; // fast cadence: one frame at 60 Hz, counted from BeginFrame
constexpr double kIdleWait   = 0.1;        // idle and minimised: ten iterations a second

struct Inputs {
	double now = 0.0;         // seconds on any monotonic clock (glfwGetTime)
	bool iconified = false;   // window minimised: never present
	bool activity = false;    // input this frame -- see ImGuiActivity()
	bool busy = false;        // caller-side reasons for the fast cadence: workers, timers, closing sequences
	bool forceRender = false; // a refresh/resize event, an atlas swap, ... since the last frame
};

// 64-bit hash of everything the renderer would consume: display size and
// offset, every vertex and index, and every command's clip/texture/offsets.
// 0 for a null or invalid ImDrawData.
uint64_t HashDrawData(const ImDrawData* dd);

// Was there input in the frame just built? Mouse movement, wheel or buttons,
// any named key held, an active item, a text field wanting input, or an open
// popup. Read after ImGui::Render() -- everything it looks at survives EndFrame.
bool ImGuiActivity();

class Pacer {
public:
	// Loop top, right after the wait/poll returned.
	void BeginFrame(double now) { frameStart_ = now; }

	// After ImGui::Render(): should this frame's draw data go to the GPU?
	bool ShouldRender(const Inputs& in, const ImDrawData* dd);

	// After the present (or the skip): how long the next wait may block. Zero
	// means poll without waiting -- the fast cadence has already been used up
	// by the frame itself.
	double WaitSeconds(double now) const;

	bool Fast() const { return fast_; }          // for the self-test
	bool PresentedOnce() const { return presentedOnce_; }

private:
	double   frameStart_    = 0.0;
	double   lastActivity_  = -1.0e9;   // "never": fast only while the caller is busy
	uint64_t lastPresented_ = 0;
	bool     presentedOnce_ = false;
	bool     hiddenSince_   = false;    // minimised at some point since the last present
	bool     iconified_     = false;
	bool     fast_          = false;
};

} // namespace FramePacing

// --frame-pacing-selftest: the decision table and the hash, headless. 0 = pass.
int RunFramePacingSelfTest(const std::wstring& exeDir);
