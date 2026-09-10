// --hang-selftest: the watchdog's own contract.
//
// Two properties matter and neither can be checked by looking at the code:
//
//   * it fires when the frame loop really stops, with a usable stack and the
//     breadcrumb that was current at the time; and
//   * IT DOES NOT FIRE OTHERWISE. A report directory that fills up with false
//     alarms is worse than no watchdog: the user stops sending the files, and
//     the one real report arrives already discounted.
//
// So the second gate (a window that still answers) gets its own case, and the
// stack capture is verified against a thread parked in a known function.
//
// Everything runs in a scratch directory under %TEMP% via PobLog::SetDirForTest
// -- HangWatch writes through PobLog::LogDir(), so that one redirect moves both
// the reports and the summary lines away from the user's real evidence.

#include "hang_watch.h"

#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string g_report;
int g_fail = 0;

void line(const std::string& s) { g_report += s; g_report += "\r\n"; }

void check(const std::string& what, bool ok, const std::string& detail = "")
{
	if (!ok) g_fail++;
	line(std::string(ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  (" + detail + ")"));
}

std::wstring ScratchDir()
{
	wchar_t tmp[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, tmp)) return L"";
	return std::wstring(tmp) + L"pobtools_hangwatch_selftest\\";
}

void Rmtree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			DeleteFileW((dir + n).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

std::vector<std::wstring> Find(const std::wstring& dir, const std::wstring& pattern)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

std::string ReadAll(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return std::string();
	std::string out;
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return out;
}

void Touch(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void Clear(const std::wstring& dir)
{
	for (const std::wstring& n : Find(dir, L"*")) DeleteFileW((dir + n).c_str());
}

// A window that answers messages, on its own thread, standing in for the POB
// window during the false-alarm case. Deliberately NOT pumped by the thread the
// watchdog is watching: that is the whole point -- a stopped frame loop with a
// live message pump is somebody dragging a window, not a hang.
struct PumpWindow {
	std::thread th;
	std::atomic<HWND> hwnd{nullptr};
	std::atomic<bool> stop{false};

	void start()
	{
		th = std::thread([this]() {
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcW;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"PobToolsHangSelfTest";
			RegisterClassExW(&wc);
			HWND h = CreateWindowExW(0, wc.lpszClassName, L"selftest", WS_OVERLAPPEDWINDOW,
			                         0, 0, 120, 60, nullptr, nullptr, wc.hInstance, nullptr);
			ShowWindow(h, SW_SHOWNA);
			hwnd.store(h);
			MSG msg;
			while (!stop.load()) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				Sleep(5);
			}
			DestroyWindow(h);
		});
		for (int i = 0; i < 200 && !hwnd.load(); i++) Sleep(5);
	}

	void finish()
	{
		stop.store(true);
		if (th.joinable()) th.join();
	}
};

// The function the stack-capture case looks for by name is irrelevant (there is
// no PDB in a release build); what matters is that the frames land inside this
// executable and that there are several of them.
void ParkedInner(std::atomic<bool>* go)
{
	while (!go->load()) Sleep(5);
}
void ParkedOuter(std::atomic<bool>* go) { ParkedInner(go); }

} // namespace

int RunHangWatchSelfTest(const std::wstring& exeDir)
{
	g_report.clear();
	g_fail = 0;
	line("PobTools hang watchdog self-test");
	line("");

	const std::wstring box = ScratchDir();
	if (box.empty()) {
		line("FAIL no temp directory");
		return 2;
	}
	Rmtree(box);
	CreateDirectoryW(box.c_str(), nullptr);
	PobLog::SetDirForTest(box);
	PobLog::ResetCapsForTest();

	// T1 -- the breadcrumb survives a round trip, and a scope puts the previous
	// one back. A breadcrumb that leaked its innermost value would point every
	// later report at whatever last finished, which is worse than none.
	{
		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 600;
		o.requireWindow = false;
		HangWatch::Start(o);
		HangWatch::Beat();
		HangWatch::Stage("outer");
		{
			HangWatch::Scope s("inner:thing");
			check("T1 the scope's tag is current", HangWatch::CurrentStageForTest() == "inner:thing",
			      HangWatch::CurrentStageForTest());
		}
		check("T1b the previous tag comes back", HangWatch::CurrentStageForTest() == "outer",
		      HangWatch::CurrentStageForTest());
		HangWatch::Stop();
		HangWatch::ResetForTest();
	}

	// T2 -- a stopped heartbeat with no window to ask really produces a report,
	// and the report says what the program was doing.
	Clear(box);
	{
		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.game = "poe1";
		o.stallMs = 600;
		o.requireWindow = false;
		HangWatch::Start(o);
		HangWatch::Beat();
		HangWatch::Stage("dict-wait");
		Sleep(2500);                       // the frame loop "stops" here
		HangWatch::Stop();

		const std::vector<std::wstring> files = Find(box, L"hang-*.txt");
		check("T2 a stalled heartbeat is reported once", files.size() == 1,
		      std::to_string(files.size()) + " file(s)");
		if (files.size() == 1) {
			const std::string body = ReadAll(box + files[0]);
			check("T2b the report names the role and game",
			      body.find("selftest (poe1)") != std::string::npos);
			check("T2c the report carries the breadcrumb",
			      body.find("dict-wait") != std::string::npos);
			check("T2d the report carries a module table",
			      body.find("pob-zh.exe") != std::string::npos &&
			          body.find("stamp=0x") != std::string::npos);
			check("T2e the file name is hang-<date>-<time>-<role>.txt",
			      files[0].size() > 20 && files[0].compare(0, 5, L"hang-") == 0 &&
			          files[0].find(L"-selftest.txt") != std::wstring::npos);
			// Frames of the watched thread. Without these the report answers
			// "it stopped" but not "where", which is the whole point.
			check("T2f the stalled thread has frames",
			      body.find("#00 ") != std::string::npos);
		}
		const std::vector<std::wstring> logs = Find(box, L"error-*.log");
		bool summary = false;
		for (const std::wstring& n : logs)
			if (ReadAll(box + n).find("[hang]") != std::string::npos) summary = true;
		check("T2g a one-line summary lands in the error log", summary);
	}

	// T3 -- THE CASE THAT MATTERS. Same stopped heartbeat, but a window that
	// still answers: dragging, resizing, a menu, a modal file dialog. Nothing
	// may be written.
	Clear(box);
	{
		PumpWindow pump;
		pump.start();
		check("T3 the test window exists", pump.hwnd.load() != nullptr);

		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 600;
		o.requireWindow = true;       // ask the window before believing the heartbeat
		HangWatch::Start(o);
		HangWatch::Beat();
		Sleep(2500);                  // heartbeat stopped for four times the threshold
		HangWatch::Stop();
		pump.finish();

		check("T3b a responsive window means no report",
		      Find(box, L"hang-*.txt").empty(),
		      std::to_string(Find(box, L"hang-*.txt").size()) + " file(s)");
	}

	// T4 -- the per-run cap. A hang that repeats says the same thing; three
	// reports is generous and unbounded growth is not an option.
	Clear(box);
	{
		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 500;
		o.requireWindow = false;
		HangWatch::Start(o);
		for (int i = 0; i < 6; i++) {
			HangWatch::Beat();          // recover, so the next stall is a new episode
			Sleep(1400);                // ... and stall again
		}
		HangWatch::Stop();
		const size_t n = Find(box, L"hang-*.txt").size();
		check("T4 at most three reports per run", n <= 3 && n >= 1, std::to_string(n));
	}

	// T5 -- the stack capture really walks somebody else's stack. A capture that
	// silently returned nothing would leave every report above passing on the
	// strength of its header alone.
	Clear(box);
	{
		std::atomic<bool> go{false};
		std::thread parked(ParkedOuter, &go);
		Sleep(150);

		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 600;
		o.requireWindow = false;
		HangWatch::Start(o);
		HangWatch::Beat();
		Sleep(2500);
		HangWatch::Stop();
		go.store(true);
		parked.join();

		const std::vector<std::wstring> files = Find(box, L"hang-*.txt");
		bool sawOther = false;
		int frameLines = 0;
		if (files.size() == 1) {
			const std::string body = ReadAll(box + files[0]);
			size_t pos = 0;
			while ((pos = body.find("#0", pos)) != std::string::npos) { frameLines++; pos += 2; }
			// The parked thread is not the main thread, so it appears under its
			// own heading -- proof the walk is not just describing the caller.
			sawOther = body.find(u8"執行緒") != std::string::npos &&
			           body.find("pob-zh.exe+0x") != std::string::npos;
		}
		check("T5 other threads are captured too", sawOther);
		check("T5b the walk produced frames", frameLines >= 2, std::to_string(frameLines));
	}

	// T6 -- the crash path writes its own report with the exception code in it.
	Clear(box);
	{
		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 60000;
		o.requireWindow = false;
		HangWatch::Start(o);
		HangWatch::Stage("lua:OnFrame");

		CONTEXT ctx{};
		RtlCaptureContext(&ctx);
		EXCEPTION_RECORD er{};
		er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
		er.ExceptionAddress = (PVOID)&RunHangWatchSelfTest;
		er.NumberParameters = 2;
		er.ExceptionInformation[0] = 1;          // write
		er.ExceptionInformation[1] = 0xdead0000;
		EXCEPTION_POINTERS ep{&er, &ctx};
		// ReportCrash, not CrashFilter: the filter chains on to whatever was
		// installed before us, and this exception is a prop, not a real one.
		HangWatch::ReportCrash(&ep);
		HangWatch::Stop();

		const std::vector<std::wstring> files = Find(box, L"crash-*.txt");
		check("T6 a crash writes a crash report", files.size() == 1,
		      std::to_string(files.size()) + " file(s)");
		if (files.size() == 1) {
			const std::string body = ReadAll(box + files[0]);
			check("T6b the report carries the exception code",
			      body.find("0xc0000005") != std::string::npos);
			check("T6c and what it tried to touch",
			      body.find("write to 0xdead0000") != std::string::npos);
			check("T6d and the breadcrumb", body.find("lua:OnFrame") != std::string::npos);
		}
		bool summary = false;
		for (const std::wstring& n : Find(box, L"error-*.log"))
			if (ReadAll(box + n).find("[crash]") != std::string::npos) summary = true;
		check("T6e a one-line summary lands in the error log", summary);
	}

	// T7 -- retention covers the new files too. Reports are bigger than log
	// lines; thirty days of them left behind would be the first thing a user
	// noticed about this feature.
	Clear(box);
	{
		Touch(box + L"hang-2000-01-01-101010-engine.txt");
		Touch(box + L"crash-2000-01-02-101010-launcher.txt");
		Touch(box + L"hang-notes.txt");                       // not ours: must survive
		Touch(box + L"error-2000-01-03.log");
		SYSTEMTIME st{};
		GetLocalTime(&st);
		wchar_t todayName[64];
		swprintf_s(todayName, L"hang-%04d-%02d-%02d-121212-engine.txt", st.wYear, st.wMonth, st.wDay);
		Touch(box + todayName);

		const int removed = PobLog::PruneOlderThan(30);
		auto exists = [&](const std::wstring& n) {
			return GetFileAttributesW((box + n).c_str()) != INVALID_FILE_ATTRIBUTES;
		};
		check("T7 stale reports and logs are removed", removed == 3,
		      "removed=" + std::to_string(removed));
		check("T7b today's report is kept", exists(todayName));
		check("T7c a file that is not ours is untouched", exists(L"hang-notes.txt"));
	}

	// T8 -- the kill switch. Somebody with a machine where suspending threads is
	// a problem has to be able to turn this off without a new build.
	Clear(box);
	{
		SetEnvironmentVariableW(L"POB_ZH_HANGWATCH", L"0");
		check("T8 POB_ZH_HANGWATCH=0 disables it", !HangWatch::Enabled());
		HangWatch::ResetForTest();
		HangWatch::Options o;
		o.role = "selftest";
		o.stallMs = 500;
		o.requireWindow = false;
		HangWatch::Start(o);           // must not start a thread at all
		Sleep(2000);
		HangWatch::Stop();
		check("T8b and nothing is written while it is off", Find(box, L"hang-*.txt").empty());
		SetEnvironmentVariableW(L"POB_ZH_HANGWATCH", nullptr);
		check("T8c it is on again once the variable is gone", HangWatch::Enabled());
	}

	// T9 -- the checkers can fail. Without this every PASS above is worth
	// nothing: a Find() that always returned empty would make T3 and T8 green.
	{
		Clear(box);
		Touch(box + L"hang-fake.txt");
		check("T9 the file finder really finds", Find(box, L"hang-*.txt").size() == 1);
		DeleteFileW((box + L"hang-fake.txt").c_str());
		check("T9b and really reports empty", Find(box, L"hang-*.txt").empty());
	}

	HangWatch::ResetForTest();
	PobLog::SetDirForTest(L"");
	Rmtree(box);

	line("");
	line(g_fail ? "RESULT FAIL" : "RESULT PASS");

	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\hang_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, g_report.data(), (DWORD)g_report.size(), &w, nullptr);
		CloseHandle(h);
	}
	return g_fail ? 2 : 0;
}

int RunHangProbe(const std::wstring& exeDir, int seconds)
{
	if (seconds < 1) seconds = 25;
	HangWatch::Options o;
	o.role = "probe";
	o.stallMs = 5000;
	o.requireWindow = false;      // there is no window here; the stall is deliberate
	HangWatch::Start(o);
	HangWatch::Beat();
	HangWatch::Stage("hang-probe");
	Sleep((DWORD)seconds * 1000);
	HangWatch::Stop();

	// Where to look, printed into the same place every other probe writes.
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	const std::wstring dir = PobLog::LogDir();
	HANDLE h = CreateFileW((exeDir + L"PobTools\\hang_probe.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		const std::string note = "hang probe done; look for hang-*-probe.txt in the log folder\r\n";
		DWORD w = 0;
		WriteFile(h, note.data(), (DWORD)note.size(), &w, nullptr);
		CloseHandle(h);
	}
	return dir.empty() ? 2 : 0;
}
