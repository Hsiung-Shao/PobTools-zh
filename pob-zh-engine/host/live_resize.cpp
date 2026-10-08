#include "live_resize.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLFW/glfw3.h>

namespace LiveResize {
namespace {

// One window per process at a time (the launcher, or a tool window after it
// has gone), and GLFW callbacks are plain function pointers.
struct Slot {
	bool* redraw = nullptr;
	std::function<void()> draw;
	Gate gate;
};
Slot g_slot;

void OnRefresh(GLFWwindow*)
{
	if (g_slot.redraw) *g_slot.redraw = true;
	if (!g_slot.draw) return;
	if (!g_slot.gate.BeginLive(InSizeMoveLoop())) return;
	g_slot.draw();
	g_slot.gate.EndLive();
}

void OnFramebufferSize(GLFWwindow*, int, int)
{
	if (g_slot.redraw) *g_slot.redraw = true;
}

} // namespace

bool InSizeMoveLoop()
{
	GUITHREADINFO gti{};
	gti.cbSize = sizeof(gti);
	return GetGUIThreadInfo(GetCurrentThreadId(), &gti) && (gti.flags & GUI_INMOVESIZE) != 0;
}

void Install(GLFWwindow* win, bool* redraw)
{
	g_slot.redraw = redraw;
	g_slot.draw = nullptr;
	g_slot.gate = Gate();
	glfwSetWindowRefreshCallback(win, OnRefresh);
	glfwSetFramebufferSizeCallback(win, OnFramebufferSize);
}

void SetDraw(std::function<void()> drawLive)
{
	g_slot.draw = std::move(drawLive);
}

void Uninstall(GLFWwindow* win)
{
	if (win) {
		glfwSetWindowRefreshCallback(win, nullptr);
		glfwSetFramebufferSizeCallback(win, nullptr);
	}
	g_slot.draw = nullptr;
	g_slot.redraw = nullptr;
}

Gate& MainGate() { return g_slot.gate; }

} // namespace LiveResize
