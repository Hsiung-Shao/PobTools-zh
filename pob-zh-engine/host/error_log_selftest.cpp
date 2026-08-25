// --error-log-selftest: the failure log's own contract.
//
// The log exists so a user's "it stopped working" turns into a place to look. If
// its lines are malformed, its retention deletes the wrong thing, or two threads
// tear each other's entries, we would find that out from the one report we
// cannot re-run. So the shape is pinned here rather than eyeballed.
//
// Everything runs in a scratch directory under %TEMP% via PobLog::SetDirForTest.
// The install's own PobTools\logs\ is the user's evidence; a self-test that
// appended to it -- or pruned it -- would be destroying the thing it protects.

#include "error_log.h"

#include "app_version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
	return std::wstring(tmp) + L"pobtools_errorlog_selftest\\";
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

std::wstring TodayName()
{
	SYSTEMTIME st{};
	GetLocalTime(&st);
	wchar_t n[64];
	swprintf_s(n, L"error-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
	return n;
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

std::vector<std::string> Lines(const std::string& body)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (size_t i = 0; i <= body.size(); i++) {
		if (i != body.size() && body[i] != '\n') continue;
		std::string s = body.substr(start, i - start);
		start = i + 1;
		while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
		if (!s.empty()) out.push_back(std::move(s));
	}
	return out;
}

bool Touch(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	WriteFile(h, "x", 1, &w, nullptr);
	CloseHandle(h);
	return true;
}

} // namespace

int RunErrorLogSelfTest(const std::wstring& exeDir)
{
	g_report.clear();
	g_fail = 0;
	line("=== error log self-test ===");

	const std::wstring box = ScratchDir();
	if (box.empty()) {
		check("a scratch directory is available", false);
		return 2;
	}
	Rmtree(box);
	CreateDirectoryW(box.c_str(), nullptr);
	PobLog::SetDirForTest(box);

	const std::wstring today = box + TodayName();

	// T1 -- the line carries the three things a bug report needs: when, which
	// version, which feature. Without the version a report is unactionable: the
	// same symptom means different things two releases apart.
	{
		DeleteFileW(today.c_str());
		PobLog::Error("app-update", "check failed: HTTP 403");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T1 one call writes exactly one line", ls.size() == 1,
		      std::to_string(ls.size()) + " line(s)");
		const std::string s = ls.empty() ? std::string() : ls[0];
		check("T1b it names the version",
		      s.find("v" POBTOOLS_VERSION_STRING) != std::string::npos, s);
		check("T1c it names the feature", s.find("[app-update]") != std::string::npos, s);
		check("T1d it keeps the message", s.find("HTTP 403") != std::string::npos, s);
		check("T1e it is timestamped", !s.empty() && s[0] == '[' && s.find("] ") != std::string::npos, s);
	}

	// T2 -- one failure is one line even when the message is not. Lua tracebacks
	// and Windows error text are multi-line; four lines in the file would read as
	// four separate failures and send whoever reads it chasing three ghosts.
	{
		DeleteFileW(today.c_str());
		PobLog::Error("inject", "patch failed\r\nstack traceback:\n\tline 2\n\tline 3");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T2 a multi-line message still writes one line", ls.size() == 1,
		      std::to_string(ls.size()) + " line(s)");
		if (!ls.empty()) {
			check("T2b the text survives, flattened",
			      ls[0].find("patch failed stack traceback: line 2 line 3") != std::string::npos,
			      ls[0]);
		}
	}

	// T3 -- an empty/odd feature tag must not produce a nameless entry. "[]" in a
	// report tells the reader nothing and looks like corruption.
	{
		DeleteFileW(today.c_str());
		PobLog::Error("", "no tag");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T3 an empty feature tag becomes [unknown], not []",
		      ls.size() == 1 && ls[0].find("[unknown]") != std::string::npos,
		      ls.empty() ? "" : ls[0]);
	}

	// T4 -- appends accumulate rather than truncate. The obvious failure mode of
	// a naive implementation is CREATE_ALWAYS, which would leave only the last
	// error of the session -- usually a consequence, not the cause.
	{
		DeleteFileW(today.c_str());
		for (int i = 0; i < 5; i++) PobLog::Error("data", "entry " + std::to_string(i));
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T4 five calls append five lines", ls.size() == 5,
		      std::to_string(ls.size()) + " line(s)");
	}

	// T4b -- ten lines per incident, then silence. A failure on a per-frame path
	// repeats sixty times a second; without a cap the file grows without bound and
	// nobody opens it, which defeats the whole feature.
	{
		DeleteFileW(today.c_str());
		PobLog::ResetCapsForTest();
		for (int i = 0; i < 500; i++) PobLog::Error("save", "the same failure over and over");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T4b 500 identical failures are capped at 10 lines", ls.size() == 10,
		      std::to_string(ls.size()) + " line(s)");
		if (ls.size() == 10) {
			// Announced silence. Without this the reader cannot tell "it stopped"
			// from "the log gave up", and those call for opposite next steps.
			check("T4b1 the tenth line says the cap was reached",
			      ls[9].find(u8"上限") != std::string::npos, ls[9]);
			check("T4b2 the earlier ones are plain", ls[0].find(u8"上限") == std::string::npos,
			      ls[0]);
		}
	}

	// T4c -- the cap must not silence a DIFFERENT failure. The dangerous version of
	// this is one that swallows the second symptom, which is usually the one that
	// explains the first.
	{
		DeleteFileW(today.c_str());
		PobLog::ResetCapsForTest();
		for (int i = 0; i < 50; i++) PobLog::Error("save", "first problem");
		PobLog::Error("data", "a completely different problem");
		for (int i = 0; i < 50; i++) PobLog::Error("save", "first problem");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T4c a different failure still gets through after another is capped",
		      ls.size() == 11, std::to_string(ls.size()) + " line(s)");
		if (ls.size() == 11) {
			check("T4c1 and it is the one that is different",
			      ls[10].find("a completely different problem") != std::string::npos, ls[10]);
		}
	}

	// T4d -- same text under a different feature tag is a different incident, so it
	// gets its own budget rather than sharing one.
	{
		DeleteFileW(today.c_str());
		PobLog::ResetCapsForTest();
		for (int i = 0; i < 20; i++) {
			PobLog::Error("save", "cannot write");
			PobLog::Error("config", "cannot write");
		}
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T4d same message, different tag, each gets its own 10", ls.size() == 20,
		      std::to_string(ls.size()) + " line(s)");
	}

	// T4e -- the cap does not reset on its own. A cap that expired on a timer would
	// let a per-frame failure write ten more lines every minute: the same unbounded
	// growth, just slower.
	{
		DeleteFileW(today.c_str());
		PobLog::ResetCapsForTest();
		for (int i = 0; i < 15; i++) PobLog::Error("save", "persistent");
		Sleep(1200);
		for (int i = 0; i < 15; i++) PobLog::Error("save", "persistent");
		const std::vector<std::string> ls = Lines(ReadAll(today));
		check("T4e the cap holds across time, it is per run", ls.size() == 10,
		      std::to_string(ls.size()) + " line(s)");
	}

	// T5 -- concurrent writers. The updater worker, the UI thread and the engine
	// all log; a torn line is worse than a missing one because it looks like data
	// corruption in whatever it was reporting.
	{
		DeleteFileW(today.c_str());
		PobLog::ResetCapsForTest();   // every thread writes a distinct message anyway
		const int kThreads = 8, kEach = 40;
		std::vector<std::thread> ts;
		for (int t = 0; t < kThreads; t++) {
			ts.emplace_back([t, kEach] {
				for (int i = 0; i < kEach; i++)
					PobLog::Error("save", "thread " + std::to_string(t) + " entry " +
					                          std::to_string(i));
			});
		}
		for (std::thread& t : ts) t.join();
		const std::vector<std::string> ls = Lines(ReadAll(today));
		int wellFormed = 0;
		for (const std::string& s : ls)
			if (s.size() > 2 && s[0] == '[' && s.find("[save] thread ") != std::string::npos)
				wellFormed++;
		check("T5 concurrent writers produce every line, none torn",
		      (int)ls.size() == kThreads * kEach && wellFormed == (int)ls.size(),
		      std::to_string(ls.size()) + " lines, " + std::to_string(wellFormed) + " well-formed");
	}

	// T6 -- retention deletes only what this module wrote, and never today's.
	// This loop runs inside the user's install directory; one careless wildcard
	// and it becomes a file shredder pointed at somebody's data.
	{
		const std::wstring keepA = box + L"important.txt";
		const std::wstring keepB = box + L"error-not-a-date.log";
		const std::wstring keepC = box + L"error-2026-08-24.log.bak";
		const std::wstring oldOne = box + L"error-2000-01-01.log";
		Touch(keepA);
		Touch(keepB);
		Touch(keepC);
		Touch(oldOne);
		PobLog::Error("data", "today stays");

		const int removed = PobLog::PruneOlderThan(30);
		auto exists = [](const std::wstring& p) {
			return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
		};
		check("T6 the stale log is removed", removed == 1 && !exists(oldOne),
		      "removed=" + std::to_string(removed));
		check("T6b today's log is kept", exists(today));
		check("T6c a non-log file is untouched", exists(keepA));
		check("T6d error-<not a date>.log is untouched", exists(keepB));
		check("T6e a name with a trailing suffix is untouched", exists(keepC));
	}

	// T7 -- keepDays 0 means "keep today only", not "delete everything". An
	// off-by-one here would throw away the log of the session being reported.
	{
		Touch(box + L"error-2000-01-02.log");
		const int removed = PobLog::PruneOlderThan(0);
		check("T7 keepDays=0 still keeps today",
		      removed == 1 &&
		          GetFileAttributesW(today.c_str()) != INVALID_FILE_ATTRIBUTES,
		      "removed=" + std::to_string(removed));
	}

	// T8 -- the checker can fail. Without this, every PASS above is worth nothing:
	// a Lines() that always returned one entry would make the file green.
	{
		const std::vector<std::string> ls = Lines("a\r\nb\r\n\r\nc");
		check("T8 the line splitter really splits", ls.size() == 3,
		      std::to_string(ls.size()));
		check("T8b and drops the blank", ls.size() == 3 && ls[2] == "c");
	}

	// T9 -- a log location that cannot be written to must not take the caller
	// down. Logging sits inside error paths; throwing from there would turn a
	// handled failure into a crash.
	{
		PobLog::SetDirForTest(L"\\\\?\\Z:\\nonexistent-volume\\logs\\");
		bool threw = false;
		try {
			PobLog::Error("panel", "unwritable location");
			PobLog::PruneOlderThan(30);
			PobLog::LogDir();
		} catch (...) {
			threw = true;
		}
		check("T9 an unwritable log location is silent, not fatal", !threw);
		PobLog::SetDirForTest(box);
	}

	PobLog::SetDirForTest(L"");
	Rmtree(box);

	line("");
	line(g_fail ? "RESULT FAIL" : "RESULT PASS");

	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\error_log_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, g_report.data(), (DWORD)g_report.size(), &w, nullptr);
		CloseHandle(h);
	}
	return g_fail ? 2 : 0;
}
