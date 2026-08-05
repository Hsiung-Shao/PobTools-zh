#include "window_dock.h"

#include "launcher_config.h"
#include "pob_launch.h"
#include "window_manager.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>   // ITaskbarList, to drop the container's taskbar button
#pragma comment(lib, "ole32.lib")

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace WindowDock {
namespace {

// Flushed on every line, on purpose. A crash loses anything still in a buffer,
// and the point of this log is to survive one: the last line written IS the last
// thing that happened. This is how the owner-crash was finally located after
// half a dozen bisections had failed.
FILE* g_log = nullptr;
bool  g_verbose = false;

void dlog(const char* fmt, ...)
{
	if (!g_log) return;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

// Per-frame chatter; silent unless verbose is on. At several thousand frames a
// second this is both enormous and slow, so it is a diagnostic, not a default.
void vlog(const char* fmt, ...)
{
	if (!g_log || !g_verbose) return;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

struct FindCtx { DWORD pid; HWND found; };

BOOL CALLBACK find_cb(HWND hwnd, LPARAM param)
{
	auto* ctx = (FindCtx*)param;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;
	// GLFW class ONLY, and no fallback. The engine creates its console window
	// before the GLFW one, so "first top-level window of that pid" adopts the
	// console -- which made POB die with an access violation.
	wchar_t cls[64] = {};
	GetClassNameW(hwnd, cls, 64);
	if (wcsncmp(cls, L"GLFW", 4) != 0) return TRUE;
	ctx->found = hwnd;
	return FALSE;
}

HWND find_window_of(DWORD pid)
{
	FindCtx ctx{ pid, nullptr };
	EnumWindows(find_cb, (LPARAM)&ctx);
	return ctx.found;
}

// One taskbar button for the whole group, not one per window.
//
// The CONTAINER's is the one to drop: a docked window's thumbnail is the real
// POB picture, while the container's would be the tab strip over a black
// rectangle (DWM renders only a window's own content, never the separate window
// sitting on top of it).
//
// ITaskbarList rather than WS_EX_TOOLWINDOW: the ex-style also restyles the
// frame and only takes effect while the window is hidden.
void taskbar_button(HWND h, bool show)
{
	ITaskbarList* tb = nullptr;
	if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_ITaskbarList, (void**)&tb)) || !tb) {
		dlog("taskbar: CoCreateInstance failed");
		return;
	}
	tb->HrInit();
	const HRESULT hr = show ? tb->AddTab(h) : tb->DeleteTab(h);
	tb->Release();
	dlog("taskbar: %s -> 0x%08X", show ? "AddTab" : "DeleteTab", (unsigned)hr);
}

} // namespace

Dock::~Dock()
{
	RestoreAll();
}

void Dock::Init(void* host, const std::wstring& logPath)
{
	host_ = host;
	if (!g_log && !logPath.empty()) {
		DeleteFileW(logPath.c_str());
		_wfopen_s(&g_log, logPath.c_str(), L"w");
	}
	// Needed by ITaskbarList. S_FALSE (already initialised) is fine.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	dlog("dock: init host=%p", host);
}

void Dock::Track(unsigned long pid, const std::wstring& label)
{
	dlog("dock: tracking pid=%lu label='%S'", pid, label.c_str());
	Tab t;
	t.pid = pid;
	t.label = label;
	tabs_.push_back(t);
	orig_.push_back(Original{});
}

void Dock::Adopt(Tab& t)
{
	HWND w = (HWND)t.hwnd;
	const size_t i = (size_t)(&t - tabs_.data());
	Original& o = orig_[i];
	o.style = GetWindowLongPtrW(w, GWL_STYLE);
	o.exStyle = GetWindowLongPtrW(w, GWL_EXSTYLE);
	RECT r{};
	GetWindowRect(w, &r);
	o.x = r.left; o.y = r.top; o.w = r.right - r.left; o.h = r.bottom - r.top;
	dlog("dock: adopting hwnd=%p style=0x%08X rect=%d,%d %dx%d",
	     (void*)w, (unsigned)o.style, o.x, o.y, o.w, o.h);

	// POB starts maximized, and a maximized window ignores SetWindowPos's size,
	// so the dock would silently do nothing.
	if (IsZoomed(w) || IsIconic(w)) {
		dlog("dock: restoring maximized/minimized window first");
		ShowWindowAsync(w, SW_RESTORE);
	}
	LONG_PTR style = o.style;
	style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
	SetWindowLongPtrW(w, GWL_STYLE, style);
	// A style change does not repaint the non-client area by itself: without
	// SWP_FRAMECHANGED the caption stays on screen even though the style says it
	// is gone.
	SetWindowPos(w, nullptr, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
	             SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
	// No SetParent and no GWLP_HWNDPARENT. Both crash; see the header.
	t.docked = true;
}

void Dock::Position(Tab& t, bool force, bool sync)
{
	HWND w = (HWND)t.hwnd;
	if (!w || !IsWindow(w)) return;
	RECT rc{};
	GetClientRect((HWND)host_, &rc);
	POINT origin{ 0, 0 };
	// Docked windows stay top-level, so they are placed in SCREEN coordinates.
	ClientToScreen((HWND)host_, &origin);
	const WindowMgr::WinRect want = WindowMgr::ComputeDockRect(
		origin.x, origin.y, rc.right - rc.left, rc.bottom - rc.top, stripH_);

	RECT cur{};
	GetWindowRect(w, &cur);
	const bool same = cur.left == want.x && cur.top == want.y &&
	                  cur.right - cur.left == want.w && cur.bottom - cur.top == want.h;
	if (!force && same) return;
	// SWP_ASYNCWINDOWPOS works here precisely because there is no parent/child
	// relationship: the input queues stay separate, which is the documented
	// precondition for this flag to post instead of send.
	//
	// While the user is dragging, though, "posted" means the docked window
	// visibly lags behind and only catches up when the drag stops. Dragging is
	// the one moment the user is watching the two windows move together, so that
	// path asks for it synchronously -- safe here for the same reason the flag
	// works at all (no attached input queues), and a window being dragged is not
	// a window that is busy recalculating.
	SetWindowPos(w, HWND_TOP, want.x, want.y, want.w, want.h,
	             SWP_NOACTIVATE | (sync ? 0u : (UINT)SWP_ASYNCWINDOWPOS));
	vlog("dock: positioned %p to %d,%d %dx%d", (void*)w, want.x, want.y, want.w, want.h);
}

void Dock::FixZOrder(Tab& t)
{
	HWND w = (HWND)t.hwnd;
	if (!w || !IsWindow(w) || !t.docked) return;
	// Sink OURS below theirs -- never try to raise theirs. Raising another
	// process's window is subject to Windows' foreground rules and simply does
	// not take: measured, HWND_TOP on the docked window ran every 200ms and never
	// once changed the order. Re-ordering our own window has no such restriction.
	SetWindowPos((HWND)host_, w, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Dock::Update(int stripH, int activeIndex)
{
	stripH_ = stripH;
	if (activeIndex != active_) {
		// ShowWindowAsync is asynchronous, so for a few moments after a switch the
		// newly active window has not caught up yet. The minimise check below must
		// not read that transient state as an intentional minimise.
		lastSwitch_ = GetTickCount64();
		active_ = activeIndex;
	}
	if (!host_) return;

	// Reap dead tabs.
	for (size_t i = 0; i < tabs_.size();) {
		Tab& t = tabs_[i];
		bool dead = false;
		if (t.hwnd && !IsWindow((HWND)t.hwnd)) dead = true;
		if (!dead && t.pid) {
			HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, t.pid);
			if (!h) dead = t.hwnd != nullptr; // gone, and it had a window before
			else {
				dead = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
				CloseHandle(h);
			}
		}
		if (dead) {
			dlog("dock: tab %zu (pid=%lu) is gone", i, t.pid);
			tabs_.erase(tabs_.begin() + (ptrdiff_t)i);
			orig_.erase(orig_.begin() + (ptrdiff_t)i);
			if (active_ >= (int)tabs_.size()) active_ = (int)tabs_.size() - 1;
			continue;
		}
		i++;
	}

	// Adopt windows that have appeared. Throttled: EnumWindows walks every
	// top-level window on the desktop, and doing that every frame of an
	// unthrottled loop is thousands of full enumerations per second.
	//
	// 60ms rather than something lazier because this interval is how long a newly
	// opened tool is visible as an ordinary window before it snaps into place --
	// and only runs at all while something is still waiting to be adopted.
	if (GetTickCount64() - lastFind_ >= 60) {
		lastFind_ = GetTickCount64();
		for (Tab& t : tabs_) {
			if (t.hwnd || !t.pid) continue;
			HWND found = find_window_of(t.pid);
			if (found) {
				t.hwnd = found;
				Adopt(t);
			}
		}
	}

	// The container is the single source of truth for "is this app on screen".
	// Docked windows follow it; they never drive it while it is minimised.
	const bool hostMin = !!IsIconic((HWND)host_);
	// Show the active one, hide the rest -- and hide everything when the
	// container is minimised, since a docked window is not a child and would
	// otherwise stay floating on the desktop on its own.
	for (size_t i = 0; i < tabs_.size(); i++) {
		Tab& t = tabs_[i];
		if (!t.hwnd || !IsWindow((HWND)t.hwnd)) continue;
		if (!hostMin && (int)i == active_) {
			const bool hidden = !IsWindowVisible((HWND)t.hwnd) || IsIconic((HWND)t.hwnd);
			if (hidden) {
				// SW_SHOWNOACTIVATE, not SW_SHOWNA: SW_SHOWNA means "display it in
				// its CURRENT state", so a window that was minimised stays minimised
				// -- and the check further down then reads that as an intentional
				// minimise and pulls the whole container down with it.
				//
				// But SW_SHOWNOACTIVATE restores it to its own most recent size and
				// position, which undoes the docking geometry. So this frame only
				// shows it; positioning happens on a later frame, once the
				// asynchronous show has actually landed. Doing both here raced, and
				// the window ended up back at its pre-dock size.
				ShowWindowAsync((HWND)t.hwnd, SW_SHOWNOACTIVATE);
			} else {
				Position(t, false);
			}
		} else {
			ShowWindowAsync((HWND)t.hwnd, SW_HIDE);
		}
	}

	// Exactly one taskbar button between the container and the docked windows,
	// and it has to follow whichever one is on screen.
	//
	// Hiding a docked window also removes ITS taskbar button. Dropping ours once
	// and for all therefore left the taskbar completely empty whenever a normal
	// tab was selected -- and, worse, whenever the container was minimised, which
	// made the whole app unreachable. Ours comes back in both of those cases.
	const bool showOwnButton = hostMin || active_ < 0 || active_ >= (int)tabs_.size();
	if (showOwnButton == droppedOwnButton_) {
		taskbar_button((HWND)host_, showOwnButton);
		droppedOwnButton_ = !showOwnButton;
	}

	// Z-order and minimise state, a few times a second. Backstop for the focus
	// callback: clicking the container's own strip while it is already focused
	// raises it WITHOUT firing a focus change.
	if (GetTickCount64() - lastKeep_ >= 200 && !hostMin) {
		lastKeep_ = GetTickCount64();
		if (active_ >= 0 && active_ < (int)tabs_.size()) {
			Tab& t = tabs_[(size_t)active_];
			if (t.hwnd && IsWindow((HWND)t.hwnd) && t.docked) {
				// A docked window minimised on its own (its taskbar button was
				// clicked) takes the container with it, so the group behaves as one
				// window.
				//
				// There is deliberately no "docked window is up but the container is
				// minimised -> restore the container" rule. That is indistinguishable
				// from the user having just minimised the container themselves, and
				// it made the window spring straight back every time they tried.
				// Restoring is Windows' job, via the taskbar button that
				// showOwnButton puts back while minimised.
				if (IsIconic((HWND)t.hwnd) && GetTickCount64() - lastSwitch_ >= 600) {
					dlog("dock: active tab minimised -> minimising container");
					ShowWindowAsync((HWND)host_, SW_MINIMIZE);
				} else if (!IsIconic((HWND)t.hwnd)) {
					FixZOrder(t);
				}
			}
		}
	}
}

void Dock::OnHostMoved()
{
	if (active_ < 0 || active_ >= (int)tabs_.size()) return;
	Tab& t = tabs_[(size_t)active_];
	if (!t.hwnd || !t.docked) return;
	// Synchronous: this fires from inside Windows' modal drag loop, and a posted
	// move visibly trails the container until the drag ends.
	Position(t, false, /*sync=*/true);
	FixZOrder(t);
}

void Dock::OnHostFocused()
{
	// Activating the container is what buries the docked window, and that happens
	// at the END of a drag or resize -- after the last move callback -- so this is
	// the one that actually matters.
	if (active_ < 0 || active_ >= (int)tabs_.size()) return;
	Tab& t = tabs_[(size_t)active_];
	if (t.hwnd && t.docked) FixZOrder(t);
}

void Dock::RequestClose(size_t index)
{
	if (index >= tabs_.size()) return;
	Tab& t = tabs_[index];
	if (!t.hwnd || !IsWindow((HWND)t.hwnd)) return;
	dlog("dock: asking tab %zu to close", index);
	// Deliberately NOT restored first. Putting the frame and the old rectangle
	// back makes the window pop out as a normal window for the moment before it
	// dies -- close several at once and they all flash across the screen.
	//
	// Restoring was only ever there in case the window persisted its geometry on
	// exit, and measurement says it does not: a full SHA-256 snapshot of POB's
	// folder before and after shows the same single file changed (Settings.xml)
	// whether it ran docked or standalone, and that file holds no window
	// geometry. RestoreAll still restores, because there the window lives on.
	//
	// PostMessage, not SendMessage: the target may be busy for seconds at a time.
	PostMessageW((HWND)t.hwnd, WM_CLOSE, 0, 0);
}

void Dock::RestoreAll()
{
	for (size_t i = 0; i < tabs_.size(); i++) {
		Tab& t = tabs_[i];
		if (!t.docked || !t.hwnd || !IsWindow((HWND)t.hwnd)) continue;
		const Original& o = orig_[i];
		dlog("dock: restoring tab %zu", i);
		SetWindowLongPtrW((HWND)t.hwnd, GWL_STYLE, (LONG_PTR)o.style);
		SetWindowLongPtrW((HWND)t.hwnd, GWL_EXSTYLE, (LONG_PTR)o.exStyle);
		SetWindowPos((HWND)t.hwnd, HWND_TOP, o.x, o.y, o.w, o.h,
		             SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
		ShowWindowAsync((HWND)t.hwnd, SW_SHOW);
		t.docked = false;
	}
	if (droppedOwnButton_ && host_) {
		// Or undocking would leave a window on screen with no way to reach it
		// from the taskbar.
		taskbar_button((HWND)host_, true);
		droppedOwnButton_ = false;
	}
}

// ---- spike -------------------------------------------------------------------

namespace {
Dock* g_spikeDock = nullptr;
}

int RunDockSpike(const std::wstring& exeDir, bool spawnPob, bool verbose)
{
	g_verbose = verbose;
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);

	LauncherConfig cfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
	InstallInfo installs = DetectInstalls(exeDir);
	const std::wstring lua = !installs.poe1Lua.empty() ? installs.poe1Lua : installs.poe2Lua;
	if (lua.empty()) {
		MessageBoxW(nullptr, L"找不到 POB 安裝，無法進行停靠驗證。", L"PobTools",
		            MB_ICONERROR | MB_OK);
		return 1;
	}

	if (!glfwInit()) return 1;
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	GLFWwindow* win = glfwCreateWindow(1500, 950, "PobTools - dock spike", nullptr, nullptr);
	if (!win) { glfwTerminate(); return 1; }
	glfwMakeContextCurrent(win);
	glfwSwapInterval(0);

	Dock dock;
	dock.Init(glfwGetWin32Window(win), exeDir + L"PobTools\\dock_log.txt");
	g_spikeDock = &dock;
	glfwSetWindowPosCallback(win, [](GLFWwindow*, int, int) {
		if (g_spikeDock) g_spikeDock->OnHostMoved();
	});
	glfwSetWindowSizeCallback(win, [](GLFWwindow*, int, int) {
		if (g_spikeDock) g_spikeDock->OnHostMoved();
	});
	glfwSetWindowFocusCallback(win, [](GLFWwindow*, int focused) {
		if (focused && g_spikeDock) g_spikeDock->OnHostFocused();
	});

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	auto spawn = [&](bool poe2) {
		const std::wstring l = poe2 ? installs.poe2Lua : installs.poe1Lua;
		if (l.empty()) return;
		const std::wstring game = poe2 ? L"poe2" : L"poe1";
		PobLaunch::SetEngineEnv(game, cfg.locale, cfg.fontFile, std::wstring());
		unsigned long pid = 0;
		if (PobLaunch::SpawnPobDetached(l, game, &pid))
			dock.Track(pid, poe2 ? L"PoE2" : L"PoE1");
	};
	if (spawnPob) spawn(installs.poe1Lua.empty());

	int active = 0;
	bool closing = false;
	while (!glfwWindowShouldClose(win) || !dock.Empty()) {
		glfwPollEvents();

		if (glfwWindowShouldClose(win) && !dock.Empty()) {
			if (!closing) {
				for (size_t i = 0; i < dock.Tabs().size(); i++) dock.RequestClose(i);
				closing = true;
			}
			glfwSetWindowShouldClose(win, GLFW_FALSE);
		}
		if (closing && dock.Empty()) glfwSetWindowShouldClose(win, GLFW_TRUE);

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##dockhost", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
		for (size_t i = 0; i < dock.Tabs().size(); i++) {
			if (i) ImGui::SameLine();
			ImGui::PushID((int)i);
			char label[64];
			snprintf(label, sizeof(label), "%s##t", i == 0 ? "Tab 1" : "Tab 2");
			const bool sel = ((int)i == active);
			if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.44f, 0.62f, 1.0f));
			if (ImGui::Button(label)) active = (int)i;
			if (sel) ImGui::PopStyleColor();
			ImGui::PopID();
		}
		if (!dock.Empty()) ImGui::SameLine();
		if (ImGui::Button("+ PoE1")) spawn(false);
		if (!installs.poe2Lua.empty()) { ImGui::SameLine(); if (ImGui::Button("+ PoE2")) spawn(true); }
		ImGui::SameLine();
		if (ImGui::Button("Close tab") && active >= 0) dock.RequestClose((size_t)active);
		ImGui::SameLine();
		if (ImGui::Button("Undock all")) dock.RestoreAll();
		const int stripH = (int)ImGui::GetCursorPosY();
		ImGui::End();

		if (active >= (int)dock.Tabs().size()) active = (int)dock.Tabs().size() - 1;
		dock.Update(stripH, active);

		ImGui::Render();
		int fw = 0, fh = 0;
		glfwGetFramebufferSize(win, &fw, &fh);
		glViewport(0, 0, fw, fh);
		glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(win);
	}

	dock.RestoreAll();
	g_spikeDock = nullptr;
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}

} // namespace WindowDock
