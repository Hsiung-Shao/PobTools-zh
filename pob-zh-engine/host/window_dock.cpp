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

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <share.h>   // _SH_DENYWR, so the log can be read while it is being written
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

// fprintf's %S goes through the C locale, which in the default "C" locale cannot
// convert anything outside ASCII -- so every Chinese tab label logged as an empty
// string, and the log said nothing about the very tabs it was meant to describe.
// UTF-8 is what the rest of this codebase writes, so the log writes it too.
std::string to_utf8_log(const std::wstring& w)
{
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
	                                  nullptr, 0, nullptr, nullptr);
	if (n <= 0) return std::string();
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
	return out;
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
		// _wfsopen with _SH_DENYWR, NOT _wfopen_s: the secure variant opens
		// EXCLUSIVELY, so nothing -- not even a plain type of the file -- could read
		// this log while the program was running. For a log whose entire purpose is
		// to survive a freeze, being readable only after the process exits defeats
		// the point: a hang is exactly when it cannot be read and exactly when it is
		// needed. Measured: every FileShare combination failed against the old handle.
		g_log = _wfsopen(logPath.c_str(), L"w", _SH_DENYWR);
	}
	// Needed by ITaskbarList. S_FALSE (already initialised) is fine.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	dlog("dock: init host=%p", host);
}

void Dock::Track(unsigned long pid, const std::wstring& label)
{
	dlog("dock: tracking pid=%lu label='%s'", pid, to_utf8_log(label).c_str());
	Tab t;
	t.pid = pid;
	t.label = label;
	// Kept so a window whose caption cannot be read still has something to show.
	t.baseLabel = label;
	tabs_.push_back(t);
	orig_.push_back(Original{});
}

bool Dock::TakeFocusRequest(size_t index)
{
	if (index >= tabs_.size() || !tabs_[index].wantFocus) return false;
	tabs_[index].wantFocus = false;
	return true;
}

void Dock::Adopt(Tab& t)
{
	HWND w = (HWND)t.hwnd;
	const size_t i = (size_t)(&t - tabs_.data());
	Original& o = orig_[i];
	// Captured ONCE, on the first attempt. Adoption can return early to wait for an
	// un-maximise, and re-reading here on the retry would record the restored state
	// as "the original" -- RestoreAll would then hand the user a merely large window
	// where they had a maximized one, and o.style would have lost WS_MAXIMIZE, which
	// is the only record that it was maximized at all.
	if (!o.captured) {
		o.style = GetWindowLongPtrW(w, GWL_STYLE);
		o.exStyle = GetWindowLongPtrW(w, GWL_EXSTYLE);
		RECT r{};
		GetWindowRect(w, &r);
		o.x = r.left; o.y = r.top; o.w = r.right - r.left; o.h = r.bottom - r.top;
		o.captured = true;
		dlog("dock: adopting hwnd=%p style=0x%08X rect=%d,%d %dx%d maximized=%d",
		     (void*)w, (unsigned)o.style, o.x, o.y, o.w, o.h, IsZoomed(w) ? 1 : 0);
	}

	// POB starts maximized, and a maximized window ignores SetWindowPos's size, so
	// the dock would silently do nothing.
	//
	// ShowWindowAsync POSTS -- it has not happened when this returns. Stripping the
	// frame in the same breath wrote `o.style` (WS_MAXIMIZE and all) back over the
	// restore that was still in flight, so the window stayed maximized as far as
	// Windows was concerned. Measured: both lines landed in the same millisecond.
	// So adoption waits instead, and the caller retries on a later frame.
	if (IsZoomed(w) || IsIconic(w)) {
		if (!t.awaitingRestore) {
			dlog("dock: window is maximized/minimized -- restoring before adopting");
			ShowWindowAsync(w, SW_RESTORE);
			t.awaitingRestore = true;
		}
		return;
	}
	t.awaitingRestore = false;
	// WS_MAXIMIZE is in this mask and used to be missing; it is not WS_MAXIMIZEBOX.
	// See WindowMgr::DockedStyleMask.
	LONG_PTR style = o.style & ~(LONG_PTR)WindowMgr::DockedStyleMask();
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

void Dock::Position(Tab& t, bool force)
{
	HWND w = (HWND)t.hwnd;
	if (!w || !IsWindow(w)) return;
	// A minimised container has no client area: Windows reports it as 0x0 at
	// (-32000,-32000), and positioning against that throws the docked window into
	// the corner at 2x2. The move and size callbacks reach here during the
	// minimise itself -- WM_MOVE and WM_SIZE are how a minimise is delivered --
	// which is BEFORE Update gets to hide anything, so the window is still on
	// screen when it happens.
	//
	// Worse than the flicker: the drag path asks for it synchronously, and a
	// synchronous SetWindowPos across processes blocks until the other side pumps
	// its queue. POB mid-recalculation is exactly when it will not, so minimising
	// could stall this loop for as long as POB took -- which from the outside is a
	// window that will not go down. Measured by S13.
	if (IsIconic((HWND)host_)) return;
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
	// ALWAYS, with no synchronous variant on offer. There was one, for the drag
	// path, and it is what let a busy POB freeze the container mid-drag. S18/S19
	// measure the difference: 516ms versus 0ms against a window that is not
	// pumping. Nothing this loop does is worth waiting on another process for.
	SetWindowPos(w, HWND_TOP, want.x, want.y, want.w, want.h,
	             SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
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
			if (!t.pid) continue;
			// Retry adoption for a window that is still waiting to come out of
			// maximized: Adopt returns early in that case and has to be called again.
			if (t.awaitingRestore && t.hwnd) { Adopt(t); continue; }
			if (t.hwnd) continue;
			HWND found = find_window_of(t.pid);
			if (found) {
				t.hwnd = found;
				Adopt(t);
			}
		}
	}

	// Tab titles follow the window. POB puts the build name in its caption
	// (`<build> (<class>) - Path of Building`), so this is what makes a tab say
	// which build it is holding. Twice a second, not per frame.
	if (GetTickCount64() - lastTitle_ >= 500) {
		lastTitle_ = GetTickCount64();
		for (Tab& t : tabs_) {
			if (!t.hwnd || !t.docked || !IsWindow((HWND)t.hwnd)) continue;
			const std::wstring title = WindowMgr::WindowTitle(t.hwnd);
			// An unreadable title keeps whatever the tab already said. A tab that
			// went blank would be worse than one that is merely out of date.
			const std::wstring shown = WindowMgr::ShortenWindowTitle(title, t.baseLabel);
			if (!shown.empty() && shown != t.label) {
				dlog("dock: tab title '%s' -> '%s'",
				     to_utf8_log(t.label).c_str(), to_utf8_log(shown).c_str());
				t.label = shown;
			}
		}
	}

	// The container is the single source of truth for "is this app on screen".
	// Docked windows follow it; they never drive it while it is minimised.
	const bool hostMin = !!IsIconic((HWND)host_);
	// Logged on the EDGE. A minimise is the one moment this log has to describe,
	// and a per-frame line at vsync would bury it under thousands of others.
	if (loggedHostMin_ != (int)hostMin) {
		loggedHostMin_ = (int)hostMin;
		dlog("dock: container %s (active=%d of %zu)",
		     hostMin ? "MINIMISED" : "restored", active_, tabs_.size());
	}
	// Show the active one, hide the rest -- and hide everything when the
	// container is minimised, since a docked window is not a child and would
	// otherwise stay floating on the desktop on its own.
	for (size_t i = 0; i < tabs_.size(); i++) {
		Tab& t = tabs_[i];
		if (!t.hwnd || !IsWindow((HWND)t.hwnd)) continue;
		WindowMgr::TabState ts;
		ts.isActive = ((int)i == active_);
		ts.hidden = !IsWindowVisible((HWND)t.hwnd) || !!IsIconic((HWND)t.hwnd);
		switch (WindowMgr::DecideTab(hostMin, ts)) {
			case WindowMgr::TabAction::Show:
				// SW_SHOWNOACTIVATE, not SW_SHOWNA: SW_SHOWNA means "display it in its
				// CURRENT state", so a window that was minimised stays minimised -- and
				// DecideHost then reads that as an intentional minimise and pulls the
				// whole container down with it.
				//
				// But SW_SHOWNOACTIVATE restores it to its own most recent size and
				// position, which undoes the docking geometry. So this frame only shows
				// it; positioning happens on a later frame, once the asynchronous show
				// has actually landed. Doing both here raced, and the window ended up
				// back at its pre-dock size.
				dlog("dock: tab %zu -> show", i);
				ShowWindowAsync((HWND)t.hwnd, SW_SHOWNOACTIVATE);
				break;
			case WindowMgr::TabAction::Position:
				Position(t, false);
				break;
			case WindowMgr::TabAction::Hide:
				// Only when there is something to hide. This used to fire unconditionally
				// every frame, which at vsync is 60 cross-process calls a second per
				// inactive tab, all of them no-ops.
				if (!ts.hidden || IsWindowVisible((HWND)t.hwnd)) {
					dlog("dock: tab %zu -> hide (%s)", i,
					     hostMin ? "container minimised" : "not the active tab");
					ShowWindowAsync((HWND)t.hwnd, SW_HIDE);
				}
				break;
		}
	}

	// Exactly one taskbar button between the container and the docked windows,
	// and it has to follow whichever one is on screen.
	const bool haveActive = active_ >= 0 && active_ < (int)tabs_.size();
	const bool showOwnButton = WindowMgr::ShouldShowOwnTaskbarButton(hostMin, haveActive);
	if (showOwnButton == droppedOwnButton_) {
		taskbar_button((HWND)host_, showOwnButton);
		droppedOwnButton_ = !showOwnButton;
	}

	// Z-order and minimise state, a few times a second. Backstop for the focus
	// callback: clicking the container's own strip while it is already focused
	// raises it WITHOUT firing a focus change.
	if (GetTickCount64() - lastKeep_ >= 200) {
		lastKeep_ = GetTickCount64();
		Tab* act = haveActive ? &tabs_[(size_t)active_] : nullptr;
		const bool actOk = act && act->hwnd && IsWindow((HWND)act->hwnd) && act->docked;
		WindowMgr::HostDecisionIn in;
		in.hostMinimised = hostMin;
		in.haveActiveTab = actOk;
		in.activeTabMinimised = actOk && !!IsIconic((HWND)act->hwnd);
		in.msSinceTabSwitch = GetTickCount64() - lastSwitch_;
		const WindowMgr::HostDecision d = WindowMgr::DecideHost(in);
		if (d.minimiseContainer) {
			dlog("dock: active tab minimised -> minimising container");
			ShowWindowAsync((HWND)host_, SW_MINIMIZE);
		} else if (d.sinkContainer) {
			FixZOrder(*act);
		}
	}
}

void Dock::OnHostMoved()
{
	if (active_ < 0 || active_ >= (int)tabs_.size()) return;
	Tab& t = tabs_[(size_t)active_];
	if (!t.hwnd || !t.docked) return;
	// Asynchronous, and throttled. Windows runs a modal loop for the whole of a
	// drag, so glfwPollEvents never returns and this callback is the only thing
	// keeping the docked window with the container.
	//
	// It used to ask synchronously, to stop the docked window trailing. That
	// bought smoothness with the wrong currency: a cross-queue SetWindowPos
	// without SWP_ASYNCWINDOWPOS SENDS, and a window whose owner is not pumping
	// cannot answer. Measured by S18/S19: 516ms against a 500ms stall, versus 0ms
	// posted. POB recalculating a build is exactly that window, so the wait landed
	// on the container -- on the thing under the user's mouse. A follower that
	// lags is a far smaller problem than a drag that judders.
	//
	// The throttle is what makes the posted version behave. A drag produces many
	// more moves than any window can repaint, and posting every one of them leaves
	// the other process a backlog of stale positions to grind through after the
	// drag has already finished. Whatever gets dropped is corrected by the first
	// Update after the modal loop ends, which is the same code that puts the
	// window back after a minimise.
	const unsigned long long now = GetTickCount64();
	if (now - lastDragPos_ >= 16) {
		lastDragPos_ = now;
		Position(t, false);
	}
	// Not throttled: it re-orders OUR window, so it costs nothing and never waits
	// on anybody.
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
		// WS_MAXIMIZE is a STATE, not a look. Writing the bit back produced a window
		// that told Windows it was maximized while sitting at the rectangle it had
		// when maximized -- overhanging the screen by the border width, and with a
		// restore button that did nothing sensible. So the bit is withheld here and
		// the state is asked for properly below.
		const bool wasMaximized = (o.style & (LONG_PTR)WS_MAXIMIZE) != 0;
		dlog("dock: restoring tab %zu (maximized=%d)", i, wasMaximized ? 1 : 0);
		SetWindowLongPtrW((HWND)t.hwnd, GWL_STYLE, (LONG_PTR)(o.style & ~(LONG_PTR)WS_MAXIMIZE));
		SetWindowLongPtrW((HWND)t.hwnd, GWL_EXSTYLE, (LONG_PTR)o.exStyle);
		if (wasMaximized) {
			// The remembered rect is the maximized one, so it is not what a restored
			// window should get; Windows works the geometry out itself from the monitor.
			SetWindowPos((HWND)t.hwnd, HWND_TOP, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED |
			             SWP_ASYNCWINDOWPOS);
			ShowWindowAsync((HWND)t.hwnd, SW_MAXIMIZE);
		} else {
			SetWindowPos((HWND)t.hwnd, HWND_TOP, o.x, o.y, o.w, o.h,
			             SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
			ShowWindowAsync((HWND)t.hwnd, SW_SHOW);
		}
		t.docked = false;
	}
	if (droppedOwnButton_ && host_) {
		// Or undocking would leave a window on screen with no way to reach it
		// from the taskbar.
		taskbar_button((HWND)host_, true);
		droppedOwnButton_ = false;
	}
}

// ---- adoption / restoration against a real window ----------------------------

namespace {
// The suite installs the launcher's own move / size / focus callbacks, and GLFW
// callbacks are plain function pointers with nowhere to put a capture.
Dock* g_selfTestDock = nullptr;
}

int RunDockStyleSelfTest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0, checks = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail = "") {
		checks++;
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};
	auto hexs = [](long long v) {
		char b[32]; snprintf(b, sizeof(b), "0x%08llX", (unsigned long long)v); return std::string(b);
	};

	if (!glfwInit()) {
		report += "FAIL could not init GLFW\nRESULT FAIL\n";
		failures++;
	} else {
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		// The container is created HIDDEN on purpose: Dock finds windows to adopt by
		// walking this process's VISIBLE GLFW windows, so a hidden one cannot be
		// mistaken for the target. Without that the dock would happily adopt itself.
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		GLFWwindow* host = glfwCreateWindow(400, 300, "dock-selftest-host", nullptr, nullptr);
		glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
		GLFWwindow* target = glfwCreateWindow(800, 600, "dock-selftest-target", nullptr, nullptr);
		if (!host || !target) {
			report += "FAIL could not create the two windows\n";
			failures++;
		} else {
			HWND th = glfwGetWin32Window(target);
			// The state that matters. POB starts like this; whether it still is by the
			// time a human runs a check is not something to leave to chance.
			ShowWindow(th, SW_MAXIMIZE);
			for (int i = 0; i < 40 && !IsZoomed(th); i++) { glfwPollEvents(); Sleep(25); }
			const LONG_PTR before = GetWindowLongPtrW(th, GWL_STYLE);
			check("S1 the target really is maximized before adoption",
			      IsZoomed(th) && (before & WS_MAXIMIZE) != 0, hexs(before));

			{
				Dock dock;
				dock.Init(glfwGetWin32Window(host), std::wstring());
				dock.Track(GetCurrentProcessId(), L"target");

				// A newly tracked window must ask to be brought to the front, ONCE.
				// Without the request the dock hides it for not being the active tab
				// and the user sees the window flash up and vanish; without the
				// one-shot it would steal focus back on every frame and the user could
				// never switch away from it.
				check("S0 a new tab asks for focus", dock.TakeFocusRequest(0));
				check("S0b ... and only once", !dock.TakeFocusRequest(0));
				check("S0c an out-of-range tab asks for nothing", !dock.TakeFocusRequest(99));
				// Adoption is throttled to 60ms and deliberately spans frames: it asks
				// for the un-maximise, then strips the frame only once that has landed.
				for (int i = 0; i < 120; i++) {
					glfwPollEvents();
					dock.Update(0, 0);
					if (!dock.Tabs().empty() && dock.Tabs()[0].docked) break;
					Sleep(25);
				}
				const bool adopted = !dock.Tabs().empty() && dock.Tabs()[0].docked;
				check("S2 the window was adopted", adopted);

				const LONG_PTR after = GetWindowLongPtrW(th, GWL_STYLE);
				// THE regression this suite exists for. WS_MAXIMIZE (0x01000000) was
				// missing from the strip list -- only WS_MAXIMIZEBOX (0x00010000) was
				// there -- so the docked window went on telling Windows it was maximized
				// while the dock sized it by hand.
				check("S3 adoption clears WS_MAXIMIZE", (after & WS_MAXIMIZE) == 0, hexs(after));
				check("S4 ... and the window is genuinely no longer zoomed", !IsZoomed(th));
				check("S5 adoption clears the caption", (after & WS_CAPTION) == 0, hexs(after));
				check("S6 adoption clears the sizing frame", (after & WS_THICKFRAME) == 0);

				// ---- the minimise cycle ------------------------------------
				//
				// Nothing else reaches this. The pure rules (M1-M12) say what
				// SHOULD happen when the container goes down; only running it
				// says whether the window actually goes with it -- and a docked
				// window left behind on the desktop is precisely what "the app
				// will not minimise" looks like from the outside.
				//
				// The container's move / size / focus callbacks are wired up the
				// way the launcher wires them, because minimising a window
				// generates WM_MOVE and WM_SIZE, and what the dock does with
				// those is part of what is under test.
				g_selfTestDock = &dock;
				glfwSetWindowPosCallback(host, [](GLFWwindow*, int, int) {
					if (g_selfTestDock) g_selfTestDock->OnHostMoved();
				});
				glfwSetWindowSizeCallback(host, [](GLFWwindow*, int, int) {
					if (g_selfTestDock) g_selfTestDock->OnHostMoved();
				});
				glfwSetWindowFocusCallback(host, [](GLFWwindow*, int focused) {
					if (focused && g_selfTestDock) g_selfTestDock->OnHostFocused();
				});

				HWND hh = glfwGetWin32Window(host);
				auto pump = [&](int frames) {
					for (int i = 0; i < frames; i++) {
						glfwPollEvents();
						dock.Update(0, 0);
						Sleep(16);
					}
				};
				auto rectOf = [](HWND w) { RECT r{}; GetWindowRect(w, &r); return r; };
				auto rectStr = [](const RECT& r) {
					char b[64];
					snprintf(b, sizeof(b), "%ld,%ld %ldx%ld", r.left, r.top,
					         r.right - r.left, r.bottom - r.top);
					return std::string(b);
				};

				// SW_SHOWNA: on screen without taking focus, so running the suite
				// cannot land a phantom click in whatever the user is doing.
				ShowWindow(hh, SW_SHOWNA);
				pump(30);
				check("S10 the docked window is on screen while the container is",
				      !!IsWindowVisible(th));
				const RECT beforeMin = rectOf(th);

				ShowWindow(hh, SW_MINIMIZE);
				pump(45);
				check("S11 the container minimises", !!IsIconic(hh));
				check("S12 ... and takes the docked window off screen with it",
				      !IsWindowVisible(th),
				      "left behind, this is what 'it will not minimise' looks like");
				const RECT duringMin = rectOf(th);
				// Minimising fires WM_MOVE (to -32000,-32000) and WM_SIZE (0x0), and
				// the dock's move handler runs before Update gets to hide anything.
				// A container with no client area has no meaningful place to put a
				// window, so nothing may be positioned against it.
				check("S13 ... without dragging it to the minimised container's rect",
				      duringMin.left == beforeMin.left && duringMin.top == beforeMin.top &&
				      duringMin.right == beforeMin.right && duringMin.bottom == beforeMin.bottom,
				      rectStr(beforeMin) + " -> " + rectStr(duringMin));

				pump(45);
				check("S14 the container stays down (nothing springs it back)",
				      !!IsIconic(hh));

				ShowWindow(hh, SW_RESTORE);
				pump(60);
				check("S15 the container comes back", !IsIconic(hh));
				check("S16 ... and so does the docked window", !!IsWindowVisible(th));
				{
					RECT hc{};
					GetClientRect(hh, &hc);
					POINT o{ 0, 0 };
					ClientToScreen(hh, &o);
					const WindowMgr::WinRect want = WindowMgr::ComputeDockRect(
						o.x, o.y, hc.right - hc.left, hc.bottom - hc.top, 0);
					const RECT got = rectOf(th);
					check("S17 ... back over the container, not wherever it was left",
					      got.left == want.x && got.top == want.y &&
					      got.right - got.left == want.w && got.bottom - got.top == want.h,
					      rectStr(got) + " want " + std::to_string(want.x) + "," +
					          std::to_string(want.y) + " " + std::to_string(want.w) + "x" +
					          std::to_string(want.h));
				}
				// Moving the container still drags the docked window along. The
				// move callback is throttled now, so a single move can be dropped
				// -- Update is what has to make that invisible, and this is the
				// check that says it does.
				{
					RECT hr{};
					GetWindowRect(hh, &hr);
					SetWindowPos(hh, nullptr, hr.left + 40, hr.top + 30, 0, 0,
					             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
					pump(30);
					RECT hc{};
					GetClientRect(hh, &hc);
					POINT o{ 0, 0 };
					ClientToScreen(hh, &o);
					const WindowMgr::WinRect want = WindowMgr::ComputeDockRect(
						o.x, o.y, hc.right - hc.left, hc.bottom - hc.top, 0);
					const RECT got = rectOf(th);
					check("S20 moving the container brings the docked window with it",
					      got.left == want.x && got.top == want.y,
					      rectStr(got) + " want " + std::to_string(want.x) + "," +
					          std::to_string(want.y));
				}
				g_selfTestDock = nullptr;

				dock.RestoreAll();
				for (int i = 0; i < 40 && !IsZoomed(th); i++) { glfwPollEvents(); Sleep(25); }
				const LONG_PTR back = GetWindowLongPtrW(th, GWL_STYLE);
				// Putting the STYLE BIT back is not the same as being maximized: that
				// produced a window overhanging the screen by the border width whose
				// restore button did nothing sensible. The state has to be asked for.
				check("S7 restoring gives back a genuinely maximized window", !!IsZoomed(th),
				      hexs(back));
				check("S8 ... with its caption", (back & WS_CAPTION) != 0, hexs(back));
			}

			// ---- what a synchronous cross-queue move actually costs ----------
			//
			// The drag path used to ask for the docked window's move
			// SYNCHRONOUSLY, on the grounds that a posted move visibly trails the
			// container. What that traded away was never measured: a window whose
			// owner is not pumping cannot answer, and the caller waits. POB
			// recalculating a build is exactly such a window, and the wait lands
			// on the container -- the one thing the user is physically dragging.
			//
			// The target lives on its own thread so the two have separate message
			// queues. That is the same condition that holds across processes, and
			// the documented precondition for SWP_ASYNCWINDOWPOS to post rather
			// than send.
			{
				const DWORD kStallMs = 500;
				std::atomic<bool> ready{ false }, stall{ false }, done{ false };
				HWND sw = nullptr;
				std::thread worker([&] {
					WNDCLASSW wc{};
					wc.lpfnWndProc = DefWindowProcW;
					wc.hInstance = GetModuleHandleW(nullptr);
					wc.lpszClassName = L"PobToolsStallTarget";
					RegisterClassW(&wc);
					sw = CreateWindowExW(0, L"PobToolsStallTarget", L"stall",
					                     WS_OVERLAPPEDWINDOW, 0, 0, 200, 150,
					                     nullptr, nullptr, wc.hInstance, nullptr);
					ready = true;   // publishes sw to the waiting thread
					while (!done) {
						if (stall) { Sleep(kStallMs); stall = false; continue; }
						MSG m;
						while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
							TranslateMessage(&m);
							DispatchMessageW(&m);
						}
						Sleep(1);
					}
					if (sw) DestroyWindow(sw);   // must be the owning thread
				});
				for (int i = 0; i < 400 && !ready; i++) Sleep(5);

				if (!sw) {
					check("S18 the stall target was created", false, "CreateWindowExW failed");
				} else {
					auto timeMove = [&](UINT extra, int xy) {
						stall = true;
						Sleep(30);   // let the worker actually enter its stall
						const ULONGLONG t0 = GetTickCount64();
						SetWindowPos(sw, nullptr, xy, xy, 0, 0,
						             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | extra);
						const long long ms = (long long)(GetTickCount64() - t0);
						while (stall) Sleep(5);
						return ms;
					};
					const long long syncMs = timeMove(0, 10);
					const long long asyncMs = timeMove(SWP_ASYNCWINDOWPOS, 20);
					check("S18 a synchronous move waits on a window that is not pumping",
					      syncMs >= (long long)kStallMs / 2,
					      std::to_string(syncMs) + " ms of a " + std::to_string(kStallMs) +
					          " ms stall");
					check("S19 ... and an asynchronous one does not",
					      asyncMs < 60, std::to_string(asyncMs) + " ms");
				}
				done = true;
				worker.join();
			}
		}
		if (target) glfwDestroyWindow(target);
		if (host) glfwDestroyWindow(host);
		glfwTerminate();
	}

	const int ran = checks;
	check("S9 the suite actually ran", ran >= 22, std::to_string(ran) + " checks");

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\dock_style_selftest.txt").c_str(),
	                       GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", report.c_str());
	return failures ? 2 : 0;
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
