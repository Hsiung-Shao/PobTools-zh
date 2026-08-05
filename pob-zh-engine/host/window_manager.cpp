#include "window_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // windows.h defines min/max as macros and eats std::max at the call site
#include <windows.h>

#include <cstdio>
#include <string>

namespace WindowMgr {

WinRect ComputeDockRect(int originX, int originY, int clientW, int clientH, int stripH)
{
	if (stripH < 0) stripH = 0;
	if (clientW < 0) clientW = 0;
	if (clientH < 0) clientH = 0;
	if (stripH > clientH) stripH = clientH; // never hand back a negative height
	return WinRect{ originX, originY + stripH, clientW, clientH - stripH };
}

bool RequestClose(void* hwnd)
{
	HWND h = (HWND)hwnd;
	if (!h || !IsWindow(h)) return false;
	// PostMessage, not SendMessage: the target belongs to another process and may
	// be busy for seconds at a time (POB recalculating), which would block the
	// caller's frame loop for as long as it took.
	return !!PostMessageW(h, WM_CLOSE, 0, 0);
}

std::wstring WindowTitle(void* hwnd)
{
	HWND h = (HWND)hwnd;
	if (!h || !IsWindow(h)) return std::wstring();
	int len = GetWindowTextLengthW(h);
	if (len <= 0) return std::wstring();
	std::wstring out((size_t)len + 1, L'\0');
	int got = GetWindowTextW(h, &out[0], len + 1);
	out.resize((size_t)(got > 0 ? got : 0));
	return out;
}

// ---- selftest ----------------------------------------------------------------

int RunWindowLayoutSelfTest(const std::wstring& exeDir)
{
	std::string report;
	int failures = 0;
	int checks = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail = "") {
		checks++;
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};
	auto rectStr = [](const WinRect& r) {
		return std::to_string(r.x) + "," + std::to_string(r.y) + " " +
		       std::to_string(r.w) + "x" + std::to_string(r.h);
	};

	{
		// The ordinary case: container client at screen (100,50), 1400x900, with a
		// 31px strip. The docked window starts below the strip and is that much
		// shorter.
		WinRect r = ComputeDockRect(100, 50, 1400, 900, 31);
		check("D1 docked rect sits below the strip", r == WinRect{ 100, 81, 1400, 869 }, rectStr(r));
	}
	{
		// Moving the container must move the rect by exactly the same amount --
		// this is what keeps a docked window glued during a drag.
		WinRect a = ComputeDockRect(100, 50, 1400, 900, 31);
		WinRect b = ComputeDockRect(340, 210, 1400, 900, 31);
		check("D2 moving the container translates the rect 1:1",
		      b.x - a.x == 240 && b.y - a.y == 160 && b.w == a.w && b.h == a.h,
		      rectStr(a) + " -> " + rectStr(b));
	}
	{
		WinRect r = ComputeDockRect(0, 0, 1400, 20, 31);
		check("D3 a container shorter than its strip yields height 0, not negative",
		      r == WinRect{ 0, 20, 1400, 0 }, rectStr(r));
	}
	{
		WinRect r = ComputeDockRect(-1200, -300, 1000, 700, 31);
		check("D4 negative screen coordinates survive (second monitor to the left)",
		      r == WinRect{ -1200, -269, 1000, 669 }, rectStr(r));
	}
	{
		WinRect r = ComputeDockRect(10, 10, 800, 600, 0);
		check("D5 a zero-height strip gives the whole client area",
		      r == WinRect{ 10, 10, 800, 600 }, rectStr(r));
	}
	{
		// Equality is what the docking loop uses to decide whether to make a
		// cross-process call at all, so it has to notice a move, not just a resize.
		WinRect a = ComputeDockRect(100, 50, 1400, 900, 31);
		WinRect b = ComputeDockRect(101, 50, 1400, 900, 31);
		check("D6 a one-pixel move compares as different", !(a == b), rectStr(b));
	}

	// Snapshot first: `checks` is incremented by the call below, so reading it
	// inside the condition would count a check that has not happened yet.
	const int ran = checks;
	check("D7 the suite actually ran", ran >= 6, std::to_string(ran) + " checks");

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\window_layout_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

} // namespace WindowMgr
