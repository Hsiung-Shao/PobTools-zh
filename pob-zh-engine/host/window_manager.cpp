#include "window_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // windows.h defines min/max as macros and eats std::max at the call site
#include <windows.h>

// For ImHashStr only: the tab-id check below has to hash the way ImGui does
// rather than by looking at the string, or it would be testing my reading of
// ImGui instead of ImGui. No context is created and nothing is drawn.
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cwchar>
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

// How long after a tab switch the "it minimised itself" rule stays disarmed.
// ShowWindowAsync has not necessarily landed yet, and the transient state looks
// exactly like a deliberate minimise.
static const unsigned long long kSwitchGraceMs = 600;

TabAction DecideTab(bool hostMinimised, const TabState& t)
{
	// The container is the single source of truth for "is this app on screen".
	// Docked windows follow it; they never drive it while it is minimised.
	if (hostMinimised || !t.isActive) return TabAction::Hide;
	// Showing and positioning are deliberately different frames. SW_SHOWNOACTIVATE
	// restores a window to its OWN last size and position, which undoes the docking
	// geometry -- so this frame only shows it and a later one moves it. Doing both
	// at once raced, and the window ended up back at its pre-dock size.
	return t.hidden ? TabAction::Show : TabAction::Position;
}

HostDecision DecideHost(const HostDecisionIn& in)
{
	HostDecision out;
	if (in.hostMinimised || !in.haveActiveTab) return out;
	if (in.activeTabMinimised) {
		// There is deliberately no "docked window is up but the container is
		// minimised -> restore the container" rule. That is indistinguishable from
		// the user having just minimised the container themselves, and it made the
		// window spring straight back every time they tried.
		out.minimiseContainer = in.msSinceTabSwitch >= kSwitchGraceMs;
	} else {
		out.sinkContainer = true;
	}
	return out;
}

bool ShouldShowOwnTaskbarButton(bool hostMinimised, bool haveActiveTab)
{
	return hostMinimised || !haveActiveTab;
}

unsigned long long DockedStyleMask()
{
	return (unsigned long long)(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
	                            WS_MAXIMIZEBOX | WS_SYSMENU | WS_MAXIMIZE);
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

std::string DockTabLabel(const std::string& utf8Label, unsigned long pid)
{
	return utf8Label + "###dock" + std::to_string(pid);
}

std::wstring ShortenWindowTitle(const std::wstring& caption, const std::wstring& fallback)
{
	if (caption.empty()) return std::wstring();   // unreadable: keep what the tab has

	std::wstring s = caption;
	// Matched as a SUFFIX rather than searched for, so a build actually called
	// "Path of Building" does not get its own name eaten out of the middle.
	static const wchar_t* kSuffix = L" - Path of Building";
	const size_t suffixLen = wcslen(kSuffix);
	if (s.size() > suffixLen && s.compare(s.size() - suffixLen, suffixLen, kSuffix) == 0) {
		s.erase(s.size() - suffixLen);
	} else if (s == L"Path of Building") {
		s.clear();   // no build loaded yet
	} else {
		// Not POB, so the caption carries nothing the tab does not already say.
		//
		// Only POB has a caption worth following: it puts the build name in it and
		// changes it as the user works. A tool's caption is fixed ("PobTools —
		// 輿圖策略"), so adopting it merely replaced a clean tab label with the same
		// words plus a redundant prefix. Taking the caption "as-is" whenever it did
		// not look like POB was the wrong rule, and this is the case it got wrong.
		return std::wstring();
	}

	// Trim: a caption is user data and can arrive with anything around it.
	const size_t b = s.find_first_not_of(L" \t");
	if (b == std::wstring::npos) s.clear();
	else s = s.substr(b, s.find_last_not_of(L" \t") - b + 1);

	if (s.empty()) return fallback;
	// A very long build name would push every other tab off the strip.
	const size_t kMax = 28;
	if (s.size() > kMax) s = s.substr(0, kMax - 1) + L"…";
	return s;
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

	// ---- what the dock decides -----------------------------------------------
	// These used to live inside Dock::Update as Win32 calls, which is why the
	// minimise behaviour could be wrong for months without anything noticing.
	{
		TabState t; t.isActive = true; t.hidden = false;
		check("M1 the active tab stays glued while the container is up",
		      DecideTab(false, t) == TabAction::Position);
		t.hidden = true;
		check("M2 an active tab that is hidden gets shown first, not positioned",
		      DecideTab(false, t) == TabAction::Show);
	}
	{
		// The one that matters for "the container will not minimise": every docked
		// window has to go away, or it is left floating on the desktop by itself --
		// a docked window is not a child, so nothing hides it automatically.
		TabState t; t.isActive = true; t.hidden = false;
		check("M3 a minimised container hides even its active tab",
		      DecideTab(true, t) == TabAction::Hide);
		t.hidden = true;
		check("M4 ... and does not try to show it again",
		      DecideTab(true, t) == TabAction::Hide);
		t.isActive = false; t.hidden = false;
		check("M5 inactive tabs are hidden whatever the container is doing",
		      DecideTab(false, t) == TabAction::Hide);
	}
	{
		HostDecisionIn in;
		in.haveActiveTab = true; in.activeTabMinimised = true; in.msSinceTabSwitch = 5000;
		check("M6 a tab minimised on its own takes the container down with it",
		      DecideHost(in).minimiseContainer);
		in.msSinceTabSwitch = 100;
		check("M7 ... but not within the grace period after a switch",
		      !DecideHost(in).minimiseContainer,
		      "ShowWindowAsync has not landed yet; that is not a user minimise");
		in.activeTabMinimised = false; in.msSinceTabSwitch = 5000;
		HostDecision d = DecideHost(in);
		check("M8 a healthy active tab keeps the container sunk below it",
		      d.sinkContainer && !d.minimiseContainer);
		in.hostMinimised = true;
		d = DecideHost(in);
		check("M9 a minimised container neither sinks nor re-minimises",
		      !d.sinkContainer && !d.minimiseContainer,
		      "or it fights the user every time they minimise it");
	}
	{
		check("M10 the container keeps its taskbar button while a normal tab shows",
		      ShouldShowOwnTaskbarButton(false, false));
		check("M11 a window tab owns the only button",
		      !ShouldShowOwnTaskbarButton(false, true));
		check("M12 a minimised container gets its button back",
		      ShouldShowOwnTaskbarButton(true, true),
		      "hiding the docked window removed ITS button; without this the app is unreachable");
	}
	{
		// WS_MAXIMIZE (0x01000000) is NOT WS_MAXIMIZEBOX (0x00010000). Mixing them up
		// left POB -- which starts maximized -- carrying a style that told Windows it
		// was still maximized while the dock sized it by hand.
		const unsigned long long m = DockedStyleMask();
		char hex[32];
		snprintf(hex, sizeof(hex), "0x%08llX", m);
		check("M13 the docked style mask drops WS_MAXIMIZE",
		      (m & 0x01000000ull) != 0, hex);
		check("M14 ... and WS_MAXIMIZEBOX as well, which is a different bit",
		      (m & 0x00010000ull) != 0);
		check("M15 ... and the caption and sizing frame",
		      (m & 0x00C00000ull) != 0 && (m & 0x00040000ull) != 0);
	}

	// ---- tab titles ----------------------------------------------------------
	{
		const std::wstring fb = L"PoE1";
		check("T1 the build name is what the tab shows",
		      ShortenWindowTitle(L"Frostblades Raider (Ranger) - Path of Building", fb) ==
		          L"Frostblades Raider (Ranger)");
		check("T2 no build loaded falls back to the tab's own name",
		      ShortenWindowTitle(L"Path of Building", fb) == fb);
		check("T3 an unreadable caption says nothing, so the tab keeps its label",
		      ShortenWindowTitle(L"", fb).empty(),
		      "empty means 'no opinion', NOT 'use the fallback'");
		// The suffix is matched at the end, not searched for: a build named after the
		// app itself must keep its name.
		check("T4 a build called 'Path of Building' survives",
		      ShortenWindowTitle(L"Path of Building - Path of Building", fb) ==
		          L"Path of Building");
		// This asserted the opposite until a tool tab showed up labelled
		// "PobTools — 輿圖策略" instead of "輿圖策略": a caption that is not POB's
		// says nothing the tab does not already say, and taking it "as-is" only
		// added a redundant prefix. The test was pinning the wrong behaviour.
		check("T5 a caption that is not POB's leaves the tab alone",
		      ShortenWindowTitle(L"PobTools — 輿圖策略", fb).empty());
		check("T5b ... including anything else that is not POB",
		      ShortenWindowTitle(L"Some Other Window", fb).empty());
		{
			const std::wstring longName(80, L'x');
			const std::wstring got = ShortenWindowTitle(longName + L" - Path of Building", fb);
			check("T6 a very long build name is clipped so the strip stays usable",
			      got.size() == 28 && got.back() == L'…', std::to_string(got.size()));
		}
		check("T7 whitespace around the name is trimmed",
		      ShortenWindowTitle(L"   Spark Inquis   - Path of Building", fb) ==
		          L"Spark Inquis",
		      "a caption is user data");
		// Whitespace is not POB's caption either, so it takes the same route as any
		// other foreign one: no opinion, and the tab keeps whatever it says. The
		// visible result is the same as falling back -- the label was already the
		// fallback -- but it also means a caption that momentarily reads blank does
		// not wipe out a build name that was there a second ago.
		check("T8 a caption of only spaces leaves the tab alone",
		      ShortenWindowTitle(L"    ", fb).empty());
		check("T8b the fallback is still used when POB has no build open",
		      ShortenWindowTitle(L"Path of Building", fb) == fb,
		      "the one case where the caption really does mean 'nothing loaded'");
	}
	{
		// Now that the label follows POB's caption, the tab's ID must not. Hashed
		// with ImGui's own function, because the whole trap is that "##" looks like
		// it hides the label from the id and does not -- only "###" does.
		const ImGuiID a = ImHashStr(DockTabLabel("Frostblades Raider (Ranger)", 4242).c_str());
		const ImGuiID b = ImHashStr(DockTabLabel("Boneshatter Jugg (Marauder)", 4242).c_str());
		check("T9 renaming the build does not give the tab a new identity", a == b,
		      "or ImGui destroys the tab, selects nothing, and the dock hides the window");
		const ImGuiID c = ImHashStr(DockTabLabel("Frostblades Raider (Ranger)", 4243).c_str());
		check("T10 ... while two windows are still two tabs", a != c);
		check("T11 the visible half is still the label",
		      DockTabLabel("PoE1", 7).rfind("PoE1###", 0) == 0,
		      DockTabLabel("PoE1", 7));
	}

	// Snapshot first: `checks` is incremented by the call below, so reading it
	// inside the condition would count a check that has not happened yet.
	const int ran = checks;
	check("D7 the suite actually ran", ran >= 32, std::to_string(ran) + " checks");

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
