#include "error_log.h"

#include "app_version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <map>
#include <mutex>

// Feature tags in use, so a new caller picks an existing one instead of adding a
// synonym. The left column is what a user would say when reporting the problem;
// grepping the log for the tag is how a report turns into a place to look.
//
//   inject       POB source/Lua patch did not apply (Chinese input, tooltips)
//   search       a search box's patch failed, or a query could not be built
//   app-update   program update: check, download, verify, extract, apply
//   data-update  translation-pack line of the same updater
//   atlas        atlas season data download/import
//   tree         passive tree data download/import
//   sig          release signature verification
//   pob          launching POB or a tool window
//   i18n         dictionary load, font load, glyph atlas degradation
//   save         writing user data (bookmarks, atlas plans, dictionaries)
//   config       pob-zh.ini read/write
//   panel        a tool panel refused to initialise
//   data         a shipped Data\*.json is missing or will not parse

namespace {

std::mutex g_mx;
std::wstring g_testDir;   // non-empty only during a self-test

// ---- per-incident cap --------------------------------------------------------
// A failure inside a per-frame path repeats sixty times a second. Reported from
// the field: with the bookmark file read-only, the regex panel's deferred save
// failed every frame and the log grew without bound -- which destroys the one
// property this file is supposed to have, that its contents are worth reading.
//
// So one incident -- the same feature tag with the same message -- is written at
// most kMaxPerIncident times per run, and after that not at all.
//
// The last one allowed says so. Silence that is not announced is ambiguous in
// the worst possible way: whoever reads the log cannot tell "the problem stopped"
// from "the log gave up", and those call for opposite next steps.
//
// The count is per process and never resets. A cap that reset on a timer would
// let a per-frame failure write ten more lines every minute, which is the same
// unbounded growth again, just slower.
const int kMaxPerIncident = 10;

std::map<std::string, int> g_seen;   // feature\x1fmessage -> times written

// The install directory, resolved once. GetModuleFileNameW(nullptr) is the exe
// even when this translation unit is linked into SimpleGraphic.dll, which is
// what we want: both modules must agree on one log location.
const std::wstring& InstallDir()
{
	static const std::wstring dir = [] {
		wchar_t buf[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
		std::wstring p(buf, n);
		const size_t slash = p.find_last_of(L'\\');
		return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
	}();
	return dir;
}

// Creates <install>\PobTools\logs\. Both levels, because a fresh install has
// neither and CreateDirectoryW does not make intermediate ones.
std::wstring EnsureDir()
{
	if (!g_testDir.empty()) {
		CreateDirectoryW(g_testDir.c_str(), nullptr);
		return g_testDir;
	}
	const std::wstring base = InstallDir();
	if (base.empty()) return std::wstring();
	CreateDirectoryW((base + L"PobTools").c_str(), nullptr);
	const std::wstring dir = base + L"PobTools\\logs\\";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

// A message is one line. GGG's strings, Windows error text and Lua tracebacks
// all contain newlines, and one failure that printed as four lines would read as
// four failures -- exactly the wrong answer to "how much went wrong?".
std::string Flatten(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	bool lastWasSpace = false;
	for (char c : s) {
		const bool isBreak = (c == '\r' || c == '\n' || c == '\t');
		if (isBreak) {
			if (!lastWasSpace && !out.empty()) out += ' ';
			lastWasSpace = true;
			continue;
		}
		out += c;
		lastWasSpace = (c == ' ');
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

} // namespace

std::wstring PobLog::LogDir()
{
	std::lock_guard<std::mutex> lk(g_mx);
	return EnsureDir();
}

void PobLog::SetDirForTest(const std::wstring& dir)
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_testDir = dir;
	if (!g_testDir.empty() && g_testDir.back() != L'\\') g_testDir += L'\\';
}

namespace {

// Appends one already-built line. Callers hold g_mx.
void WriteLine(const std::wstring& dir, const std::string& line)
{
	// FILE_SHARE_WRITE matters: app_update.cpp's older logger omits it, so a
	// second running copy of PobTools silently loses every line it tries to log.
	SYSTEMTIME st{};
	GetLocalTime(&st);
	wchar_t name[64];
	swprintf_s(name, L"error-%04d-%02d-%02d.log", st.wYear, st.wMonth, st.wDay);
	HANDLE h = CreateFileW((dir + name).c_str(), FILE_APPEND_DATA,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;   // deliberately silent; see the header
	DWORD wrote = 0;
	WriteFile(h, line.data(), (DWORD)line.size(), &wrote, nullptr);
	CloseHandle(h);
}

std::string Stamp()
{
	SYSTEMTIME st{};
	GetLocalTime(&st);
	char head[64];
	const int n = sprintf_s(head, "[%04d-%02d-%02d %02d:%02d:%02d] ",
	                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	return std::string(head, n > 0 ? (size_t)n : 0);
}

} // namespace

void PobLog::ResetCapsForTest()
{
	std::lock_guard<std::mutex> lk(g_mx);
	g_seen.clear();
}

void PobLog::Error(const char* feature, const std::string& msg)
{
	if (!feature || !*feature) feature = "unknown";
	std::lock_guard<std::mutex> lk(g_mx);

	const std::wstring dir = EnsureDir();
	if (dir.empty()) return;

	const std::string flat = Flatten(msg);
	const std::string key = std::string(feature) + '\x1f' + flat;

	int& times = g_seen[key];
	if (times >= kMaxPerIncident) return;   // this incident has said enough
	times++;

	// The whole line is assembled first and written with ONE WriteFile. Appends
	// under FILE_APPEND_DATA are atomic per call, so a single write is what keeps
	// two writers (host + engine, or two running copies) from interleaving.
	std::string line = Stamp() + "v" POBTOOLS_VERSION_STRING " [" + feature + "] " + flat;
	if (times == kMaxPerIncident)
		line += u8"（已達 " + std::to_string(kMaxPerIncident) + u8" 次上限，這一則之後不再記錄）";
	line += "\r\n";
	WriteLine(dir, line);
}

int PobLog::PruneOlderThan(int keepDays)
{
	if (keepDays < 0) return 0;
	std::lock_guard<std::mutex> lk(g_mx);
	const std::wstring dir = EnsureDir();
	if (dir.empty()) return 0;

	// The cutoff, as the local date `keepDays` ago. Compared as text, because the
	// file name is already a sortable date and converting it back to a time just
	// adds a timezone question nobody needs answered.
	SYSTEMTIME now{};
	GetLocalTime(&now);
	FILETIME ft{};
	if (!SystemTimeToFileTime(&now, &ft)) return 0;
	ULARGE_INTEGER u{};
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	u.QuadPart -= (unsigned long long)keepDays * 24ull * 60ull * 60ull * 10000000ull;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;
	SYSTEMTIME cut{};
	if (!FileTimeToSystemTime(&ft, &cut)) return 0;
	wchar_t cutName[32];
	swprintf_s(cutName, L"%04d-%02d-%02d", cut.wYear, cut.wMonth, cut.wDay);

	// Only names of exactly the shape this module writes. Never a wildcard sweep:
	// this directory is inside the user's install, and a delete loop that trusted
	// "*.log" would one day meet a file somebody else put there.
	auto isOurName = [](const std::wstring& fn, std::wstring* date) {
		if (fn.size() != 20) return false;                       // error-YYYY-MM-DD.log
		if (fn.compare(0, 6, L"error-") != 0) return false;
		if (fn.compare(16, 4, L".log") != 0) return false;
		for (int i = 6; i < 16; i++) {
			const wchar_t c = fn[i];
			const bool wantDash = (i == 10 || i == 13);
			if (wantDash ? (c != L'-') : (c < L'0' || c > L'9')) return false;
		}
		if (date) *date = fn.substr(6, 10);
		return true;
	};

	int removed = 0;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"error-*.log").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		std::wstring date;
		if (!isOurName(fd.cFileName, &date)) continue;
		if (date >= cutName) continue;           // today's file is never older
		if (DeleteFileW((dir + fd.cFileName).c_str())) removed++;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return removed;
}
