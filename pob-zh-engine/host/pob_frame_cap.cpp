#include "pob_frame_cap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace PobFrameCap {

int ClampFps(int fps)
{
	if (fps <= 0 || fps > kMaxFps) return 0;
	return fps;
}

int EffectiveFps(const CapInputs& in)
{
	int cap = ClampFps(in.active ? in.foregroundFps : in.backgroundFps);
	if (cap == 0 && !in.presented) {
		// Nothing waited on vsync this frame: pace at the monitor rate so an
		// uncapped, static window does not spin.
		cap = in.refreshHz > 0 ? ClampFps(in.refreshHz) : 60;
		if (cap == 0) cap = 60;
	}
	return cap;
}

double SleepSeconds(const CapInputs& in)
{
	const int fps = EffectiveFps(in);
	if (fps <= 0) return 0.0;
	const double budget = 1.0 / fps;
	double spent = in.now - in.frameStart;
	if (spent < 0.0) spent = 0.0;          // clock went backwards: treat as instant
	const double left = budget - spent;
	if (left <= 0.0) return 0.0;
	return left > budget ? budget : left;
}

bool ShouldPresent(const PresentInputs& in)
{
	if (!in.elided) return true;
	if (in.firstFrame || in.sizeChanged || in.screenshot || in.imguiContent) return true;
	if (in.now - in.lastPresent >= kPresentKeepAlive) return true;
	if (in.now < in.lastPresent) return true; // clock went backwards
	return false;
}

void PreciseSleep(double seconds)
{
	if (seconds <= 0.0) return;
	if (seconds > 1.0) seconds = 1.0; // a cap never asks for more; guard against a bad clock
	static thread_local HANDLE timer = [] {
		HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		return h; // null on systems without high-resolution timers
	}();
	if (timer) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)(seconds * 1.0e7); // relative, 100 ns units
		if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
			WaitForSingleObject(timer, INFINITE);
			return;
		}
	}
	timeBeginPeriod(1);
	Sleep((DWORD)(seconds * 1000.0 + 0.5));
	timeEndPeriod(1);
}

} // namespace PobFrameCap
