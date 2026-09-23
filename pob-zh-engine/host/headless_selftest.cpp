// --headless-selftest: can POB's own Lua run in our engine with no window, be
// driven over stdio, and give the same numbers POB writes into a saved build?
//
// Runs against a SANDBOX copy of the detected PoE1 install, never the user's:
// POB writes Settings.xml, Update\ and (on load) Builds\ next to itself in
// standalone mode, and the update check we run here downloads into Update\.
// TreeData (555 MB) and Data\TimelessJewelData (237 MB) are junctioned because
// nothing in this test writes them; everything else is copied so the update
// check's integrity pass sees every manifest file where it expects it.

#include "headless_proc.h"
#include "modern_ui_browser.h"
#include "paste_fixtures_poe2.h"
#include "pob_launch.h"

#include "error_log.h"
#include "launcher_config.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::string g_rep;
int g_fail = 0;
int g_pass = 0;

void line(const std::string& s) { g_rep += s; g_rep += "\r\n"; }

void check(const std::string& what, bool ok, const std::string& detail = "")
{
	if (ok) g_pass++; else g_fail++;
	line(std::string(ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  (" + detail + ")"));
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring widen(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

bool IsDir(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsReparse(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Removes a tree. A junction is removed as a directory entry and NEVER
// descended into: descending would delete the user's TreeData through it.
void Rmtree(const std::wstring& dir)
{
	if (!IsDir(dir)) return;
	if (IsReparse(dir)) { RemoveDirectoryW(dir.c_str()); return; }
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			const std::wstring p = dir + L"\\" + n;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				Rmtree(p);
			} else {
				SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(p.c_str());
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

bool MkdirP(const std::wstring& dir)
{
	// Each level explicitly: CreateDirectoryW does not create parents.
	for (size_t i = 3; i < dir.size(); i++) {
		if (dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
	}
	CreateDirectoryW(dir.c_str(), nullptr);
	return IsDir(dir);
}

// NTFS junction (mount point): no privilege needed, unlike a symlink.
bool MakeJunction(const std::wstring& link, const std::wstring& target)
{
	if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
	HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
	                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	std::wstring subst = L"\\??\\" + target;
	if (subst.back() == L'\\') subst.pop_back();
	const std::wstring print = target.back() == L'\\' ? target.substr(0, target.size() - 1) : target;
	// REPARSE_DATA_BUFFER's MountPointReparseBuffer, laid out by hand: the
	// struct lives in ntifs.h, which user-mode code does not get to include.
	const size_t substBytes = subst.size() * sizeof(wchar_t);
	const size_t printBytes = print.size() * sizeof(wchar_t);
	const size_t pathBytes = substBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
	std::vector<char> buf(8 + 8 + pathBytes, 0);
	char* p = buf.data();
	*(DWORD*)(p + 0) = IO_REPARSE_TAG_MOUNT_POINT;
	*(WORD*)(p + 4) = (WORD)(8 + pathBytes); // ReparseDataLength
	*(WORD*)(p + 8) = 0;                     // SubstituteNameOffset
	*(WORD*)(p + 10) = (WORD)substBytes;     // SubstituteNameLength
	*(WORD*)(p + 12) = (WORD)(substBytes + sizeof(wchar_t)); // PrintNameOffset
	*(WORD*)(p + 14) = (WORD)printBytes;     // PrintNameLength
	memcpy(p + 16, subst.data(), substBytes);
	memcpy(p + 16 + substBytes + sizeof(wchar_t), print.data(), printBytes);
	DWORD ret = 0;
	BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr);
	CloseHandle(h);
	if (!ok) RemoveDirectoryW(link.c_str());
	return ok != 0;
}

// Copies src\ into dst\ recursively. `rel` is the path relative to the sandbox
// root, used to decide what to junction and what to skip.
struct CopyStats { int files = 0; int dirs = 0; int junctions = 0; int errors = 0; };

bool ShouldJunction(const std::wstring& rel)
{
	return _wcsicmp(rel.c_str(), L"TreeData") == 0 ||
	       _wcsicmp(rel.c_str(), L"Data\\TimelessJewelData") == 0;
}

bool ShouldSkip(const std::wstring& rel)
{
	// State this test owns (Builds gets one sample; Settings.xml is regenerated)
	// or that a previous POB run left behind.
	return _wcsicmp(rel.c_str(), L"Builds") == 0 ||
	       _wcsicmp(rel.c_str(), L"Update") == 0 ||
	       _wcsicmp(rel.c_str(), L"Settings.xml") == 0 ||
	       _wcsicmp(rel.c_str(), L"imgui.ini") == 0;
}

void CopyTree(const std::wstring& src, const std::wstring& dst, const std::wstring& rel, CopyStats& st)
{
	MkdirP(dst);
	st.dirs++;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
		if (ShouldSkip(r)) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (ShouldJunction(r)) {
				// Wine has no mount-point reparse points (FSCTL_SET_REPARSE_POINT
				// fails there): copy the folder instead -- slower, same content.
				if (MakeJunction(dst + L"\\" + n, src + L"\\" + n)) st.junctions++;
				else if (PobLaunch::RunningUnderWine()) { RemoveDirectoryW((dst + L"\\" + n).c_str()); CopyTree(src + L"\\" + n, dst + L"\\" + n, r, st); }
				else st.errors++;
			} else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
				CopyTree(src + L"\\" + n, dst + L"\\" + n, r, st);
			}
		} else {
			if (CopyFileW((src + L"\\" + n).c_str(), (dst + L"\\" + n).c_str(), FALSE)) st.files++; else st.errors++;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

std::string ReadFileA(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	std::string out;
	char buf[65536];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return out;
}

// Runs a console program with the given working directory and waits for it;
// returns false on spawn failure or timeout. Used for POB's own Update.exe.
static bool RunAndWait(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                       unsigned timeoutMs, unsigned long* exitCode)
{
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) return false;
	CloseHandle(pi.hThread);
	bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
	if (!done) TerminateProcess(pi.hProcess, 1);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	if (exitCode) *exitCode = code;
	return done;
}

bool WriteFileA(const std::wstring& path, const std::string& data)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
	CloseHandle(h);
	return w == data.size();
}

// <PlayerStat stat="X" value="Y"/> pairs, in document order.
std::vector<std::pair<std::string, std::string>> ParsePlayerStats(const std::string& xml)
{
	std::vector<std::pair<std::string, std::string>> out;
	size_t pos = 0;
	for (;;) {
		pos = xml.find("<PlayerStat ", pos);
		if (pos == std::string::npos) break;
		size_t end = xml.find('>', pos);
		if (end == std::string::npos) break;
		std::string tag = xml.substr(pos, end - pos);
		auto attr = [&](const char* name) -> std::string {
			std::string key = std::string(name) + "=\"";
			size_t a = tag.find(key);
			if (a == std::string::npos) return {};
			a += key.size();
			size_t b = tag.find('"', a);
			return b == std::string::npos ? std::string() : tag.substr(a, b - a);
		};
		std::string stat = attr("stat"), value = attr("value");
		if (!stat.empty()) out.emplace_back(stat, value);
		pos = end;
	}
	return out;
}

std::string ManifestVersion(const std::string& manifest)
{
	// <Version number="2.67.2" .../> on PoE1; PoE2 writes platform/branch first
	size_t tag = manifest.find("<Version ");
	if (tag == std::string::npos) return {};
	size_t end = manifest.find('>', tag);
	size_t a = manifest.find(" number=\"", tag);
	if (a == std::string::npos || (end != std::string::npos && a > end)) return {};
	a += strlen(" number=\"");
	size_t b = manifest.find('"', a);
	return b == std::string::npos ? std::string() : manifest.substr(a, b - a);
}

std::string JsonScalarToString(const json& v)
{
	if (v.is_string()) return v.get<std::string>();
	if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
	if (v.is_number_integer()) return std::to_string(v.get<long long>());
	if (v.is_number_float()) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%.14g", v.get<double>());
		return buf;
	}
	return v.dump();
}

// "same value" the way Lua's tostring and a double round-trip allow: relative
// tolerance for numbers, exact for anything else.
bool SameValue(const std::string& xmlValue, const json& v, double relTol)
{
	if (v.is_number()) {
		char* endp = nullptr;
		double x = strtod(xmlValue.c_str(), &endp);
		if (endp && *endp == '\0') {
			double y = v.get<double>();
			double scale = std::fmax(std::fabs(x), std::fabs(y));
			return std::fabs(x - y) <= relTol * std::fmax(scale, 1.0);
		}
	}
	return JsonScalarToString(v) == xmlValue;
}

// How many oracle entries disagree with `stats`; fills `examples` with the first few.
int CompareStats(const std::vector<std::pair<std::string, std::string>>& oracle, const json& stats,
                 double relTol, int& missing, std::vector<std::string>& examples)
{
	int bad = 0;
	missing = 0;
	for (const auto& [stat, value] : oracle) {
		if (!stats.contains(stat)) {
			missing++;
			if (examples.size() < 8) examples.push_back(stat + ": missing (xml=" + value + ")");
			continue;
		}
		if (!SameValue(value, stats[stat], relTol)) {
			bad++;
			if (examples.size() < 8) examples.push_back(stat + ": xml=" + value + " bridge=" + JsonScalarToString(stats[stat]));
		}
	}
	return bad;
}

std::wstring FindSampleBuild(const std::wstring& buildsDir)
{
	// The most recently written .xml that carries <PlayerStat: an empty or
	// half-written build would make the oracle vacuous, and the newest one is
	// the likeliest to have been saved by the POB version now installed, which
	// is what makes the soft (on-disk) comparison mean anything.
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((buildsDir + L"\\*.xml").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return L"";
	std::wstring found;
	ULARGE_INTEGER newest{};
	do {
		ULARGE_INTEGER t;
		t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
		t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		if (!found.empty() && t.QuadPart <= newest.QuadPart) continue;
		std::string body = ReadFileA(buildsDir + L"\\" + fd.cFileName);
		if (body.find("<PlayerStat ") != std::string::npos && body.find("<Build ") != std::string::npos) {
			found = fd.cFileName;
			newest = t;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return found;
}

} // namespace

// Developer probe: `method|method:{"json":...}|...`, each answered in order,
// all responses written as one JSON array. No sandbox on purpose -- this is
// for looking at what the bridge says about a real install, not for changing it.
int RunBridgeCall(const std::wstring& exeDir, const std::wstring& pobDir,
                  const std::wstring& methods, const std::wstring& outFile)
{
	std::wstring dir = pobDir;
	while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
	HeadlessProc::Options opt;
	opt.exeDir = exeDir;
	opt.launchLua = dir + L"\\Launch.lua";
	opt.game = L"poe1";
	opt.locale = L"zh-rTW";
	{
		wchar_t env[2048] = {};
		if (GetEnvironmentVariableW(L"POB_ZH_BRIDGE", env, 2048)) opt.bridgeLua = env;
		if (GetEnvironmentVariableW(L"POB_GAME", env, 2048) && env[0]) opt.game = env;
		if (GetEnvironmentVariableW(L"POB_LOCALE", env, 2048) && env[0]) opt.locale = env;
	}
	HeadlessProc::Child child;
	std::string err;
	json out = json::array();
	int rc = 0;
	if (!child.Start(opt, err)) {
		out.push_back(json{{"error", err}});
		rc = 2;
	} else {
		json hello;
		if (!child.WaitEvent("hello", hello, 90000)) {
			out.push_back(json{{"error", "no hello"}, {"stray", child.StrayOutput()}});
			rc = 2;
		} else {
			std::string spec = narrow(methods);
			size_t pos = 0;
			while (pos <= spec.size()) {
				size_t bar = spec.find('|', pos);
				std::string one = spec.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
				pos = bar == std::string::npos ? spec.size() + 1 : bar + 1;
				if (one.empty()) continue;
				std::string method = one, params = "{}";
				size_t colon = one.find(':');
				if (colon != std::string::npos) { method = one.substr(0, colon); params = one.substr(colon + 1); }
				// `@file` reads the JSON from a file: PowerShell strips the quotes
				// out of an inline JSON argument before the process ever sees it.
				if (!params.empty() && params[0] == '@') params = ReadFileA(widen(params.substr(1)));
				json p;
				try { p = json::parse(params); } catch (...) { p = json::object(); }
				json r;
				// POB's own long jobs (the weighted trade query, the timeless
				// jewel search) run well past two minutes on a big build
				bool ok = child.Call(method, p, r, 600000);
				out.push_back(json{{"method", method}, {ok ? "result" : "error", r}});
				if (!ok) rc = 1;
			}
		}
		child.Stop(5000);
	}
	std::wstring path = outFile.empty() ? exeDir + L"bridge_call.json" : outFile;
	WriteFileA(path, out.dump(2));
	return rc;
}

int RunHeadlessSelfTest(const std::wstring& exeDir, const std::wstring& pobDirOverride)
{
	g_rep.clear(); g_fail = 0; g_pass = 0;
	const std::wstring reportPath = exeDir + L"headless_selftest.txt";
	DeleteFileW(reportPath.c_str()); // never let a stale report be read as this run's

	line("PobTools --headless-selftest");

	// --- locate the PoE1 install -------------------------------------------
	std::wstring pobDir = pobDirOverride;
	if (pobDir.empty()) {
		InstallInfo info = DetectInstalls(exeDir);
		pobDir = info.poe1Dir;
	}
	while (!pobDir.empty() && pobDir.back() == L'\\') pobDir.pop_back();
	check("PoE1 install found", !pobDir.empty() && IsDir(pobDir), narrow(pobDir));
	if (pobDir.empty() || !IsDir(pobDir)) {
		line("no PoE1 Path of Building install beside the exe; pass its folder as the second argument");
		goto done;
	}
	{
		// --- sandbox ---------------------------------------------------------
		const std::wstring sandboxRoot = exeDir + L"PobTools\\sandbox";
		const std::wstring sandbox = sandboxRoot + L"\\PathOfBuildingCommunity";
		Rmtree(sandbox);
		MkdirP(sandboxRoot);
		CopyStats st;
		CopyTree(pobDir, sandbox, L"", st);
		check("sandbox copied", st.errors == 0 && st.files > 100,
		      "files=" + std::to_string(st.files) + " dirs=" + std::to_string(st.dirs) +
		          " junctions=" + std::to_string(st.junctions) + " errors=" + std::to_string(st.errors));
		check("sandbox TreeData is a junction (not a copy; under Wine, which has no junctions, a copy)",
		      IsReparse(sandbox + L"\\TreeData") || (PobLaunch::RunningUnderWine() && IsDir(sandbox + L"\\TreeData")));

		const std::wstring sample = FindSampleBuild(pobDir + L"\\Builds");
		check("a saved build with <PlayerStat> exists to use as the oracle", !sample.empty(), narrow(sample));
		MkdirP(sandbox + L"\\Builds");
		if (!sample.empty()) {
			CopyFileW((pobDir + L"\\Builds\\" + sample).c_str(), (sandbox + L"\\Builds\\" + sample).c_str(), FALSE);
		}
		const std::string manifestVersion = ManifestVersion(ReadFileA(sandbox + L"\\manifest.xml"));
		check("manifest.xml has a Version number", !manifestVersion.empty(), manifestVersion);

		// --- boot the headless engine ---------------------------------------
		HeadlessProc::Options opt;
		opt.exeDir = exeDir;
		opt.launchLua = sandbox + L"\\Launch.lua";
		opt.game = L"poe1";
		opt.locale = L"zh-rTW";
		{
			wchar_t env[2048] = {};
			if (GetEnvironmentVariableW(L"POB_ZH_BRIDGE", env, 2048)) opt.bridgeLua = env;
		}
		HeadlessProc::Child child;
		std::string err;
		check("headless child spawned", child.Start(opt, err), err);

		json hello;
		bool gotHello = child.WaitEvent("hello", hello, 90000);
		check("hello event within 90 s", gotHello, gotHello ? hello.dump() : child.StrayOutput());
		if (gotHello) {
			check("hello.pobVersion == manifest Version",
			      hello.value("pobVersion", "") == manifestVersion,
			      hello.value("pobVersion", "") + " vs " + manifestVersion);
		}
		json gate;
		bool gotGate = child.WaitEvent("gate_result", gate, 5000);
		check("gate_result event", gotGate, gotGate ? gate.dump() : "");
		check("gate_result names the POB version it was taken against (the host remembers it per version)",
		      gotGate && !gate.value("pobVersion", "").empty(), gotGate ? gate.value("pobVersion", "") : "");
		if (gotGate) {
			check("compatibility gate ok (every probe found its POB surface)",
			      gate.value("ok", false) && gate.value("checked", 0) > 0, gate.dump());
		}

		json ver;
		bool okVer = child.Call("version", json::object(), ver, 30000);
		check("version call", okVer, ver.dump());
		std::string buildPath = okVer ? ver.value("buildPath", "") : "";
		check("version.buildPath points into the sandbox",
		      !buildPath.empty() && widen(buildPath).find(L"sandbox") != std::wstring::npos, buildPath);

		// --- load a real build ----------------------------------------------
		json loaded;
		bool okLoad = false;
		if (!sample.empty() && !buildPath.empty()) {
			std::string path = buildPath + narrow(sample);
			okLoad = child.Call("load_build_file", json{{"path", path}}, loaded, 120000);
		}
		check("load_build_file", okLoad, loaded.dump().substr(0, 400));

		json stats;
		bool okStats = okLoad && child.Call("get_stats", json::object(), stats, 30000);
		check("get_stats returns scalars", okStats && stats.value("count", 0) > 20,
		      okStats ? "count=" + std::to_string(stats.value("count", 0)) : stats.dump());

		// --- hard oracle: POB's own save vs the bridge's read -----------------
		json saved;
		bool okSave = okLoad && child.Call("save_xml", json::object(), saved, 60000);
		check("save_xml", okSave && saved.value("xml", "").find("<PlayerStat ") != std::string::npos,
		      okSave ? "" : saved.dump());
		if (okSave && okStats) {
			auto oracle = ParsePlayerStats(saved["xml"].get<std::string>());
			int missing = 0;
			std::vector<std::string> ex;
			int bad = CompareStats(oracle, stats["stats"], 1e-9, missing, ex);
			std::string detail = "checked=" + std::to_string(oracle.size()) + " bad=" + std::to_string(bad) +
			                     " missing=" + std::to_string(missing);
			for (auto& e : ex) detail += "; " + e;
			check("hard oracle: every <PlayerStat> POB saves equals get_stats (rel 1e-9)",
			      oracle.size() > 20 && bad == 0 && missing == 0, detail);
		}

		// --- soft oracle: the build as last saved by whichever POB saved it ---
		if (okStats && !sample.empty()) {
			auto oracle = ParsePlayerStats(ReadFileA(pobDir + L"\\Builds\\" + sample));
			int missing = 0;
			std::vector<std::string> ex;
			int bad = CompareStats(oracle, stats["stats"], 1e-3, missing, ex);
			std::string detail = "checked=" + std::to_string(oracle.size()) + " differ=" + std::to_string(bad) +
			                     " missing=" + std::to_string(missing);
			for (auto& e : ex) detail += "; " + e;
			// Informational: the file may have been saved by another POB version.
			line(std::string("INFO soft oracle (on-disk <PlayerStat> vs recalculated, rel 1e-3): ") + detail);
		}

		// --- sidebar -----------------------------------------------------------
		json sidebar;
		bool okSide = okLoad && child.Call("get_sidebar", json::object(), sidebar, 30000);
		int rows = okSide ? (int)sidebar["rows"].size() : 0;
		int textRows = 0, zhRows = 0;
		if (okSide) {
			for (auto& r : sidebar["rows"]) {
				if (r.contains("lhs")) {
					textRows++;
					if (r.value("lhs", "") != r.value("lhsRaw", "")) zhRows++;
				}
			}
		}
		check("get_sidebar has rows with text", okSide && textRows > 10,
		      "rows=" + std::to_string(rows) + " withText=" + std::to_string(textRows) + " translated=" + std::to_string(zhRows));
		check("sidebar rows come back translated (zh-rTW dictionaries applied)", zhRows > 0);

		// --- F2's switch: the page's way back to POB's original English ----------
		// Nothing the bridge answers with carries an English copy of a breakdown
		// line, so "show the original" can only mean asking again with the
		// engine's display translation off -- the same flag the classic window's
		// F2 flips. Proven on the sidebar, where each row carries both forms.
		{
			json off, on, verOff;
			const bool okOff = child.Call("set_translate", json{{"enabled", false}}, off, 30000);
			json sideOff;
			const bool okSideOff = okOff && child.Call("get_sidebar", json::object(), sideOff, 30000);
			int offText = 0, offSame = 0;
			if (okSideOff)
				for (auto& r : sideOff["rows"])
					if (r.contains("lhs")) {
						offText++;
						if (r.value("lhs", "") == r.value("lhsRaw", "")) offSame++;
					}
			const bool okVerOff = child.Call("version", json::object(), verOff, 30000);
			const bool okOn = child.Call("set_translate", json{{"enabled", true}}, on, 30000);
			json sideOn;
			const bool okSideOn = okOn && child.Call("get_sidebar", json::object(), sideOn, 30000);
			int onZh = 0;
			if (okSideOn)
				for (auto& r : sideOn["rows"])
					if (r.contains("lhs") && r.value("lhs", "") != r.value("lhsRaw", "")) onZh++;
			check("set_translate{false} gives POB's original English back, {true} restores the translation",
			      okOff && off.value("enabled", true) == false && okSideOff && offText > 10 && offSame == offText &&
			          okVerOff && verOff.value("translate", true) == false &&
			          okOn && on.value("enabled", false) == true && okSideOn && onZh > 0,
			      "off: " + std::to_string(offSame) + "/" + std::to_string(offText) +
			          " rows equal their raw, version.translate=" +
			          (okVerOff ? verOff["translate"].dump() : "?") + "; on: " + std::to_string(onZh) + " translated");
			check("the translation switch is a capability the page can test for",
			      okVerOff && verOff["caps"].value("translateToggle", false), "");
		}

		// --- sidebar 1b: stat keys, breakdown, build list, build info ------------
		{
			int valueRows = 0, withStat = 0, doubleSpacer = 0, firstBd = -1;
			bool prevSpacer = false;
			if (okSide) {
				int i = 0;
				for (auto& r : sidebar["rows"]) {
					i++;
					bool spacer = !r.contains("lhs") && !r.contains("rhs");
					if (spacer && prevSpacer) doubleSpacer++;
					prevSpacer = spacer;
					if (r.contains("rhs")) {
						valueRows++;
						if (r.contains("stat") && r["stat"].is_string()) withStat++;
					}
					if (firstBd < 0 && r.value("hasBreakdown", false)) firstBd = i;
				}
			}
			// The per-entry wrapper is what puts `stat` on a row; POB's own rows
			// never have it. Below 80% the sidebar would regroup wrongly.
			check("sidebar value rows carry POB's stat key (per-entry wrapper)",
			      valueRows > 10 && withStat * 10 >= valueRows * 8,
			      "valueRows=" + std::to_string(valueRows) + " withStat=" + std::to_string(withStat));
			check("per-entry wrapper did not change POB's spacer logic (no double spacer)", okSide && doubleSpacer == 0,
			      "doubleSpacer=" + std::to_string(doubleSpacer));
			// The breakdown exists where POB has GetSidebarBreakdown (beta); on
			// master the rows must say there is nothing to open, and asking
			// anyway must answer empty rather than fail.
			json capv;
			const bool okCaps = child.Call("version", json::object(), capv, 30000) && capv.contains("caps");
			const bool canBd = okCaps && capv["caps"].value("sidebarBreakdown", false);
			json bd;
			if (canBd) {
				bool okBd = firstBd > 0 && child.Call("sidebar_breakdown", json{{"rowIndex", firstBd}}, bd, 30000);
				check("sidebar_breakdown of the first breakdown row has sections (POB with GetSidebarBreakdown)",
				      okBd && bd.contains("sections") && !bd["sections"].empty(),
				      okBd ? bd.dump().substr(0, 300) : ("row=" + std::to_string(firstBd) + " " + bd.dump()));
			} else {
				bool okBd = okCaps && child.Call("sidebar_breakdown", json{{"rowIndex", 1}}, bd, 30000);
				check("sidebar breakdown degrades on a POB without GetSidebarBreakdown: no row offers one, asking answers empty",
				      okCaps && firstBd < 0 && okBd && bd.contains("sections") && bd["sections"].empty(),
				      "caps=" + (okCaps ? capv["caps"].dump() : std::string("?")) + " firstBd=" + std::to_string(firstBd) + " " + bd.dump().substr(0, 160));
			}
		}
		{
			json lb;
			bool okLb = child.Call("list_builds", json::object(), lb, 30000);
			bool found = false;
			if (okLb) {
				for (auto& e : lb["entries"]) {
					if (widen(e.value("fileName", "")) == sample) found = true;
				}
			}
			check("list_builds lists the sample build via POB's BuildListHelpers", okLb && found,
			      okLb ? "entries=" + std::to_string(lb["entries"].size()) : lb.dump());
			json bi;
			bool okBi = okLoad && child.Call("get_build_info", json::object(), bi, 30000);
			check("get_build_info has points parsed from POB's point display",
			      okBi && bi["points"].value("usedMax", 0) >= 99 && bi["points"].value("ascMax", 0) == 8 &&
			          bi["points"].value("used", -1) >= 0 && !bi.value("className", "").empty(),
			      okBi ? bi["points"].dump() : bi.dump());
		}

		// --- tree 1c/1d: data, art, state, hover, click round trip, undo ---------
		{
			json td;
			bool okTd = okLoad && child.Call("tree_data", json::object(), td, 120000);
			int nodeCount = okTd ? td.value("nodeCount", 0) : 0;
			int withXY = 0, withFrames = 0;
			if (okTd) {
				for (auto& [k, n] : td["nodes"].items()) {
					if (n.contains("x") && n["x"].is_number() && n.contains("y")) withXY++;
					if (n.contains("frames")) withFrames++;
				}
			}
			check("tree_data: POB laid out >2500 nodes, every one with x/y",
			      okTd && nodeCount > 2500 && withXY == nodeCount && td["connectors"].size() > 2000 && td["groups"].size() > 700,
			      okTd ? "nodes=" + std::to_string(nodeCount) + " xy=" + std::to_string(withXY) + " frames=" + std::to_string(withFrames) +
			                 " connectors=" + std::to_string(td["connectors"].size()) + " groups=" + std::to_string(td["groups"].size())
			           : td.dump().substr(0, 300));
			check("tree_data: frame names resolved for most nodes", okTd && withFrames * 10 >= nodeCount * 8);

			json ta;
			bool okTa = okLoad && child.Call("tree_assets", json::object(), ta, 60000);
			int files = 0, missingFiles = 0;
			if (okTa) {
				for (auto& [file, sz] : ta["sheets"].items()) {
					files++;
					if (GetFileAttributesW((sandbox + L"\\" + widen(file)).c_str()) == INVALID_FILE_ATTRIBUTES) missingFiles++;
				}
			}
			check("tree_assets: every sheet file exists under the install, frame + class art named",
			      okTa && files > 10 && missingFiles == 0 && ta["assets"].contains("PSSkillFrame") && ta["assets"].contains("centerwitch") &&
			          ta.value("missingSheets", json::array()).empty(),
			      okTa ? "files=" + std::to_string(files) + " missing=" + std::to_string(missingFiles) + " assets=" + std::to_string(ta["assets"].size())
			           : ta.dump().substr(0, 300));

			json ts;
			bool okTs = okLoad && child.Call("get_tree_state", json::object(), ts, 30000);
			int allocCount = okTs ? (int)ts["allocatedNodes"].size() : 0;
			check("get_tree_state: allocated list matches its count and the points POB counted",
			      okTs && allocCount > 50 && ts.value("allocCount", -1) == allocCount &&
			          ts["points"].value("used", -1) + ts["points"].value("ascUsed", 0) + 2 <= allocCount + 4,
			      okTs ? "alloc=" + std::to_string(allocCount) + " points=" + ts["points"].dump() : ts.dump().substr(0, 300));

			// Cluster subgraphs come with POB's own node ids (never the array
			// index), their own connectors, and every socket with its jewel.
			{
				int dyn = 0, dynStatic = 0, dynFramed = 0, conns = 0, socketsN = 0, withJewel = 0, withRadius = 0, overlays = 0;
				std::set<long long> staticIds;
				if (okTd) for (auto& [k, n] : td["nodes"].items()) staticIds.insert(n.value("id", -1LL));
				if (okTs) {
					for (auto& d : ts["dynamicNodes"]) {
						dyn++;
						long long id = d.value("id", -1LL);
						// inner cluster sockets legitimately reuse the tree's expansion socket ids
						if (id < 65536 && d.value("type", "") != "Socket") dynStatic++;
						if (d.contains("frames") && d["frames"].is_object()) dynFramed++;
					}
					conns = (int)ts.value("dynamicConnectors", json::array()).size();
					for (auto& s : ts["sockets"]) {
						socketsN++;
						if (s.contains("itemId")) withJewel++;
						if (s.contains("radiusIndex")) withRadius++;
						if (s.contains("overlay")) overlays++;
					}
				}
				check("get_tree_state: cluster nodes carry POB's ids (none collide with the static tree), POB frames and connectors",
				      okTs && dyn > 0 && dynStatic == 0 && dynFramed == dyn && conns >= dyn,
				      "dyn=" + std::to_string(dyn) + " staticIdCollisions=" + std::to_string(dynStatic) + " framed=" + std::to_string(dynFramed) + " connectors=" + std::to_string(conns));
				check("get_tree_state: every socket listed; socketed jewels carry overlay art and (for radius jewels) the radius index",
				      okTs && socketsN >= 20 && withJewel > 0 && overlays == withJewel && withRadius >= 1 && ts.value("jewelRadius", json::array()).size() >= 5,
				      "sockets=" + std::to_string(socketsN) + " jewels=" + std::to_string(withJewel) + " overlays=" + std::to_string(overlays) + " radius=" + std::to_string(withRadius));
				check("tree_assets: jewel radius ring images resolved (Assets/ and TreeData/)",
				      okTa && ta.contains("images") && ta["images"].contains("ring") && ta["images"].contains("jewelShadedOuterRing") && ta["images"].contains("maraketh1"),
				      okTa ? ta.value("images", json::object()).dump().substr(0, 200) : "");
			}

			// A node one step outside the allocated set: an unallocated neighbour
			// of an allocated node whose own neighbours are allocated.
			long long target = -1;
			if (okTd && okTs) {
				std::set<long long> alloc;
				for (auto& id : ts["allocatedNodes"]) alloc.insert(id.get<long long>());
				for (auto& [k, n] : td["nodes"].items()) {
					long long id = n.value("id", -1LL);
					if (alloc.count(id) || n.value("type", "") != "Normal" || n.contains("asc")) continue;
					for (auto& l : n["linked"]) {
						if (alloc.count(l.get<long long>())) { target = id; break; }
					}
					if (target >= 0) break;
				}
			}
			json hv;
			bool okHv = target >= 0 && child.Call("node_hover", json{{"id", target}}, hv, 30000);
			check("node_hover: an adjacent unallocated node costs 1 point along POB's path",
			      okHv && hv.value("cost", -1) == 1 && hv["path"].size() == 1 && !hv.value("allocated", true),
			      okHv ? hv.dump() : "target=" + std::to_string(target) + " " + hv.dump());

			json ni;
			bool okNi = target >= 0 && child.Call("node_info", json{{"id", target}}, ni, 30000);
			check("node_info: POB's tooltip lines come through translated",
			      okNi && ni["lines"].size() >= 2 && !ni.value("nameZh", "").empty(),
			      okNi ? ni.dump().substr(0, 200) : ni.dump().substr(0, 300));

			// click on, click off: state and stats must return to where they were
			json before;
			child.Call("get_stats", json::object(), before, 30000);
			json c1;
			bool okC1 = target >= 0 && child.Call("tree_click", json{{"id", target}}, c1, 60000);
			bool nowAlloc = okC1 && c1.contains("allocatedNodes");
			if (nowAlloc) {
				nowAlloc = false;
				for (auto& id : c1["allocatedNodes"]) if (id.get<long long>() == target) nowAlloc = true;
			}
			check("tree_click allocates the node (POB's AllocNode + recalculation)",
			      okC1 && nowAlloc && c1["allocatedNodes"].size() == (size_t)allocCount + 1 && c1.value("rev", 0) > ts.value("rev", 0),
			      okC1 ? "alloc=" + std::to_string(c1["allocatedNodes"].size()) + " rev=" + std::to_string(c1.value("rev", 0)) : c1.dump().substr(0, 300));
			json u1;
			bool okU1 = okC1 && child.Call("tree_undo", json::object(), u1, 60000);
			check("tree_undo restores the allocation count", okU1 && u1["allocatedNodes"].size() == (size_t)allocCount,
			      okU1 ? "alloc=" + std::to_string(u1["allocatedNodes"].size()) : u1.dump().substr(0, 300));
			json r1;
			bool okR1 = okU1 && child.Call("tree_redo", json::object(), r1, 60000);
			check("tree_redo re-applies it", okR1 && r1["allocatedNodes"].size() == (size_t)allocCount + 1);
			json c2;
			bool okC2 = okR1 && child.Call("tree_click", json{{"id", target}}, c2, 60000);
			json after;
			child.Call("get_stats", json::object(), after, 30000);
			check("tree_click again deallocates and the stats return to the original values",
			      okC2 && c2["allocatedNodes"].size() == (size_t)allocCount && before.contains("stats") && after.contains("stats") &&
			          before["stats"].value("Life", 0.0) == after["stats"].value("Life", 1.0) &&
			          before["stats"].value("TotalDPS", 0.0) == after["stats"].value("TotalDPS", 1.0),
			      okC2 ? "alloc=" + std::to_string(c2["allocatedNodes"].size()) : c2.dump().substr(0, 300));

			// The allocated node with the most dependents (one next to the class
			// start takes the whole tree with it): dealloc, undo, and the set
			// must be identical, cluster-jewel nodes included. POB's own undo
			// base is taken before cluster subgraphs exist, so without the
			// bridge's ResetUndo this loses every cluster node.
			long long hub = -1;
			size_t hubDeps = 0;
			if (okTs) {
				for (auto& idj : ts["allocatedNodes"]) {
					long long id = idj.get<long long>();
					if (id >= 65536) continue;
					json h;
					if (child.Call("node_hover", json{{"id", id}}, h, 30000) && h.contains("depends") && h["depends"].size() > hubDeps) {
						hubDeps = h["depends"].size();
						hub = id;
					}
				}
			}
			std::set<long long> origSet;
			for (auto& idj : ts.value("allocatedNodes", json::array())) origSet.insert(idj.get<long long>());
			size_t clusterCount = 0;
			for (long long id : origSet) if (id >= 65536) clusterCount++;
			json hc, hu;
			bool okHc = hub >= 0 && child.Call("tree_click", json{{"id", hub}}, hc, 60000);
			bool okHu = okHc && child.Call("tree_undo", json::object(), hu, 60000);
			std::set<long long> undoSet;
			for (auto& idj : hu.value("allocatedNodes", json::array())) undoSet.insert(idj.get<long long>());
			json afterHub;
			child.Call("get_stats", json::object(), afterHub, 30000);
			check("dealloc the hub node then undo: the allocated set is identical (cluster nodes too) and DPS matches",
			      okHu && hc["allocatedNodes"].size() < origSet.size() && undoSet == origSet &&
			          before["stats"].value("TotalDPS", 0.0) == afterHub["stats"].value("TotalDPS", 1.0),
			      "hub=" + std::to_string(hub) + " deps=" + std::to_string(hubDeps) + " afterClick=" + std::to_string(hc.value("allocatedNodes", json::array()).size()) +
			          " afterUndo=" + std::to_string(undoSet.size()) + "/" + std::to_string(origSet.size()) + " cluster=" + std::to_string(clusterCount));

			// Class / ascendancy switching: Build.lua's classDrop / ascendDrop
			// callbacks, with POB's confirm question when points would be lost.
			{
				json cl;
				bool okCl = child.Call("list_classes", json::object(), cl, 30000);
				long long curClass = okCl ? cl["current"].value("classId", -1LL) : -1;
				long long curAsc = okCl ? cl["current"].value("ascendClassId", -1LL) : -1;
				check("list_classes: seven classes, numeric ascendancy indices, current selection named",
				      okCl && cl["classes"].size() == 7 && cl["classes"][0]["ascendancies"].size() >= 2 &&
				          cl["classes"][0]["ascendancies"][0]["id"].is_number() && !cl["current"].value("className", "").empty(),
				      okCl ? cl["current"].dump() : cl.dump().substr(0, 300));
				long long other = -1;
				if (okCl) for (auto& c : cl["classes"]) { long long id = c.value("id", -1LL); if (id != curClass) { other = id; break; } }
				json q;
				bool okQ = other >= 0 && child.Call("set_class", json{{"classId", other}}, q, 60000);
				check("set_class with points on the tree asks (class_change) instead of resetting",
				      okQ && q.value("needsConfirm", "") == "class_change" && q.value("classId", -1LL) == other, q.dump().substr(0, 200));
				json rs;
				bool okRs = okQ && child.Call("set_class", json{{"classId", other}, {"confirm", "reset"}}, rs, 60000);
				// SelectClass leaves only the start nodes (class + ascendancy starts) allocated: zero points used
				check("set_class confirm=reset: the tree is reset to the new class (no points used)",
				      okRs && rs.value("classId", -1LL) == other && rs["allocatedNodes"].size() < 8 && rs["points"].value("used", -1) == 0 &&
				          rs.value("allocCount", -1) == (int)rs["allocatedNodes"].size(),
				      okRs ? "class=" + std::to_string(rs.value("classId", -1LL)) + " alloc=" + std::to_string(rs["allocatedNodes"].size()) + " used=" + std::to_string(rs["points"].value("used", -1)) : rs.dump().substr(0, 200));
				json u1;
				bool okU1c = okRs && child.Call("tree_undo", json::object(), u1, 60000);
				std::set<long long> undoSet2;
				for (auto& idj : u1.value("allocatedNodes", json::array())) undoSet2.insert(idj.get<long long>());
				json stU;
				child.Call("get_stats", json::object(), stU, 30000);
				check("tree_undo after the class change restores the class, the allocated set and the DPS",
				      okU1c && u1.value("classId", -1LL) == curClass && undoSet2 == origSet &&
				          before["stats"].value("TotalDPS", 0.0) == stU["stats"].value("TotalDPS", 1.0),
				      "class=" + std::to_string(u1.value("classId", -1LL)) + " alloc=" + std::to_string(undoSet2.size()) + "/" + std::to_string(origSet.size()));
				long long otherAsc = -1;
				if (okCl) for (auto& c : cl["classes"]) {
					if (c.value("id", -1LL) != curClass) continue;
					for (auto& a : c["ascendancies"]) { long long id = a.value("id", -1LL); if (id > 0 && id != curAsc) { otherAsc = id; break; } }
				}
				json as;
				bool okAs = otherAsc > 0 && child.Call("set_ascendancy", json{{"ascendClassId", otherAsc}}, as, 60000);
				json stA;
				child.Call("get_stats", json::object(), stA, 30000);
				json ub;
				bool okUb = okAs && child.Call("tree_undo", json::object(), ub, 60000);
				json stB;
				child.Call("get_stats", json::object(), stB, 30000);
				check("set_ascendancy switches the ascendancy (stats move) and tree_undo brings it back",
				      okAs && as.value("ascendClassId", -1LL) == otherAsc &&
				          (stA["stats"].value("TotalDPS", 0.0) != stU["stats"].value("TotalDPS", 0.0) || stA["stats"].value("Life", 0.0) != stU["stats"].value("Life", 0.0)) &&
				          okUb && ub.value("ascendClassId", -1LL) == curAsc && stB["stats"].value("TotalDPS", 0.0) == stU["stats"].value("TotalDPS", 1.0),
				      "asc " + std::to_string(curAsc) + "->" + std::to_string(otherAsc) + " dps " + std::to_string(stU["stats"].value("TotalDPS", 0.0)) + "/" +
				          std::to_string(stA["stats"].value("TotalDPS", 0.0)) + "/" + std::to_string(stB["stats"].value("TotalDPS", 0.0)));
			}

			// a reachable, unallocated mastery: click asks, choose, undo
			long long mastery = -1;
			if (okTd) {
				std::set<long long> alloc;
				for (auto& id : c2.value("allocatedNodes", json::array())) alloc.insert(id.get<long long>());
				for (auto& [k, n] : td["nodes"].items()) {
					long long id = n.value("id", -1LL);
					if (alloc.count(id) || n.value("type", "") != "Mastery" || !n.contains("masteryEffects")) continue;
					for (auto& l : n["linked"]) {
						if (alloc.count(l.get<long long>())) { mastery = id; break; }
					}
					if (mastery >= 0) break;
				}
			}
			json mc;
			bool okMc = mastery >= 0 && child.Call("tree_click", json{{"id", mastery}}, mc, 60000);
			bool asks = okMc && mc.value("needsMastery", false) && mc.contains("effects") && !mc["effects"].empty();
			check("tree_click on an unallocated mastery asks for an effect", asks,
			      okMc ? mc.dump().substr(0, 200) : "mastery=" + std::to_string(mastery) + " " + mc.dump().substr(0, 200));
			json ms;
			bool okMs = asks && child.Call("select_mastery", json{{"id", mastery}, {"effect", mc["effects"][0]["effect"]}}, ms, 60000);
			bool masteryAlloc = false;
			if (okMs) for (auto& id : ms["allocatedNodes"]) if (id.get<long long>() == mastery) masteryAlloc = true;
			check("select_mastery allocates the mastery with that effect (TreeTab:SaveMasteryPopup)",
			      okMs && masteryAlloc && ms["overrides"].contains(std::to_string(mastery)),
			      okMs ? "alloc=" + std::to_string(ms["allocatedNodes"].size()) : ms.dump().substr(0, 300));
			json mu;
			bool okMu = okMs && child.Call("tree_undo", json::object(), mu, 60000);
			check("tree_undo removes the mastery again", okMu && mu["allocatedNodes"].size() == (size_t)allocCount);

			// tattoos (PoE1): a keystone takes one, Reset Node removes it
			std::set<long long> allocSet;
			if (okTs) for (auto& v : ts["allocatedNodes"]) allocSet.insert(v.get<long long>());
			long long strNode = -1, plainNotable = -1;
			if (okTd) for (auto& [k, n] : td["nodes"].items()) {
				const long long nid = n.value("id", -1LL);
				if (strNode < 0 && n.value("type", "") == "Keystone" && !n.contains("asc")) strNode = nid;
				if (plainNotable < 0 && n.value("type", "") == "Notable" && !n.contains("asc") && n["stats"].size() > 0 &&
				    n["stats"][0].get<std::string>().find("to Strength") == std::string::npos && n["stats"][0].get<std::string>().find("to Dexterity") == std::string::npos &&
				    n["stats"][0].get<std::string>().find("to Intelligence") == std::string::npos) plainNotable = nid;
			}
			json to, tap, tst, trs, tst2, tno;
			bool okTo = strNode >= 0 && child.Call("tattoo_options", json{{"id", strNode}}, to, 60000);
			bool okTap = okTo && to.value("allowed", false) && !to["options"].empty() &&
			             child.Call("tattoo_apply", json{{"id", strNode}, {"tattoo", to["options"][0]["id"]}}, tap, 60000) &&
			             child.Call("get_tree_state", json::object(), tst, 30000);
			bool tattooed = okTap && tst["overrides"].contains(std::to_string(strNode)) && tst["overrides"][std::to_string(strNode)].value("why", "") == "tattoo";
			bool okTrs = tattooed && child.Call("tattoo_apply", json{{"id", strNode}, {"reset", true}}, trs, 60000) && child.Call("get_tree_state", json::object(), tst2, 30000);
			bool okTno = plainNotable >= 0 && child.Call("tattoo_options", json{{"id", plainNotable}}, tno, 60000);
			check("tattoo_options/tattoo_apply: POB's Replace Modifier dialog tattoos a keystone and Reset Node restores it; a plain notable is not offered",
			      tattooed && okTrs && !tst2["overrides"].contains(std::to_string(strNode)) && okTno && !tno.value("allowed", true),
			      "str=" + std::to_string(strNode) + " " + (okTo ? to.dump().substr(0, 160) : to.dump().substr(0, 200)) + " " + tap.dump().substr(0, 120));

			// Compare: the other tree's allocation comes with the state; unticking drops it
			json cmp1, cmp0;
			bool okCmp = child.Call("set_compare_spec", json{{"index", 1}}, cmp1, 60000) && child.Call("set_compare_spec", json::object(), cmp0, 60000);
			check("set_compare_spec ticks Compare (its allocated nodes come with the tree state) and unticks it",
			      okCmp && cmp1.contains("compare") && cmp1["compare"]["allocatedNodes"].size() > 0 && !cmp0.contains("compare"),
			      cmp1.value("compare", json()).dump().substr(0, 120));

			// Shift-traced path: two unallocated linked nodes next to the tree, allocated along that path
			long long p1 = -1, p2 = -1;
			if (okTd) {
				for (auto& [k, n] : td["nodes"].items()) {
					const long long a = n.value("id", -1LL);
					if (allocSet.count(a) || n.contains("asc") || n.value("type", "") != "Normal") continue;
					bool nextToTree = false;
					for (auto& l : n["linked"]) if (allocSet.count(l.get<long long>())) nextToTree = true;
					if (!nextToTree) continue;
					for (auto& l : n["linked"]) {
						const long long bId = l.get<long long>();
						const auto& bn = td["nodes"].value(std::to_string(bId), json());
						if (!allocSet.count(bId) && bn.is_object() && bn.value("type", "") == "Normal" && !bn.contains("asc")) { p1 = a; p2 = bId; break; }
					}
					if (p1 >= 0) break;
				}
			}
			json tr1, trU, trBad;
			bool okTr = p1 >= 0 && child.Call("tree_click", json{{"id", p2}, {"trace", json::array({p1, p2})}}, tr1, 60000);
			bool both = false;
			if (okTr) {
				int hits = 0;
				for (auto& v : tr1["allocatedNodes"]) if (v.get<long long>() == p1 || v.get<long long>() == p2) hits++;
				both = hits == 2;
			}
			bool okTrU = okTr && child.Call("tree_undo", json::object(), trU, 60000);
			bool okTrBad = p1 >= 0 && child.Call("tree_click", json{{"id", p2}, {"trace", json::array({p2, p1})}}, trBad, 60000);
			check("tree_click{path}: a Shift-traced path is allocated as traced; a path not ending at the node is refused",
			      both && okTrU && trU["allocatedNodes"].size() == (size_t)allocCount && !okTrBad,
			      "p1=" + std::to_string(p1) + " p2=" + std::to_string(p2) + " " + tr1.dump().substr(0, 120) + " " + trBad.dump().substr(0, 120));

			// Show Node Power: the heat map's per-node power and the Power Report
			json np0, np1, npOff;
			const auto t0np = GetTickCount64();
			bool okNp = child.Call("node_power", json{{"maxDepth", 3}}, np0, 600000);
			std::string powerStat;
			if (okNp) for (auto& st : np0["stats"]) if (powerStat.empty() && st.contains("stat")) powerStat = st.value("stat", "");
			bool okNp1 = okNp && !powerStat.empty() && child.Call("node_power", json{{"stat", powerStat}, {"maxDepth", 3}}, np1, 600000);
			bool okOff = child.Call("node_power", json{{"enabled", false}}, npOff, 60000);
			bool powered = okNp && np0["nodes"].size() > 50 && np0["powerMax"].value("offence", 0.0) > 0.0;
			bool reported = okNp1 && np1["report"].size() > 10 && np1["report"][0].contains("powerStr") && np1["report"][0].value("nameZh", "") != "";
			check("node_power runs POB's PowerBuilder: every reachable node gets a power, a chosen stat gives its Power Report, and it can be switched off",
			      powered && reported && okOff && !npOff.value("enabled", true),
			      "nodes=" + std::to_string(np0["nodes"].size()) + " report=" + std::to_string(okNp1 ? np1["report"].size() : 0) +
			          " stat=" + powerStat + " ms=" + std::to_string(GetTickCount64() - t0np));

			// node tooltip with stat differences (Ctrl+D)
			json nd0, nd1;
			bool okNd = p2 >= 0 && child.Call("node_info", json{{"id", p2}}, nd0, 60000) && child.Call("node_info", json{{"id", p2}, {"diff", true}}, nd1, 60000);
			check("node_info{diff} adds POB's stat comparison lines to the node tooltip",
			      okNd && nd1["lines"].size() > nd0["lines"].size(),
			      okNd ? std::to_string(nd0["lines"].size()) + " -> " + std::to_string(nd1["lines"].size()) : nd1.dump().substr(0, 200));
		}

		// --- 1e: the build's passive trees: list, copy/rename/move/delete, links, reset, convert ---
		{
			json ls0;
			bool okLs0 = okLoad && child.Call("list_specs", json::object(), ls0, 30000);
			const size_t n0 = okLs0 ? ls0["specs"].size() : 0;
			const int act0 = okLs0 ? ls0.value("activeSpec", 1) : 1;
			const int pts0 = (okLs0 && n0 > 0) ? ls0["specs"][act0 - 1].value("points", -1) : -1;
			check("list_specs lists the build's trees with the active one, its points and the version choices",
			      okLs0 && n0 >= 1 && pts0 > 0 && ls0["versions"].size() >= 1 && ls0["specs"][act0 - 1].value("active", false),
			      ls0.dump().substr(0, 300));
			json cp, rn, mv;
			bool okCp = okLs0 && child.Call("spec_op", json{{"op", "copy"}, {"index", act0}, {"title", "bridge copy"}}, cp, 60000);
			bool okRn = okCp && child.Call("spec_op", json{{"op", "rename"}, {"index", (int)n0 + 1}, {"title", "bridge renamed"}}, rn, 60000);
			bool okMv = okRn && child.Call("spec_op", json{{"op", "move"}, {"index", (int)n0 + 1}, {"to", 1}}, mv, 60000);
			bool listOk = okMv && mv["specs"].size() == n0 + 1 && mv["specs"][0].value("title", "") == "bridge renamed" &&
			              mv["specs"][0].value("points", -1) == pts0 && mv.value("activeSpec", 0) == act0 + 1;
			json badName;
			bool okBad = child.Call("spec_op", json{{"op", "rename"}, {"index", 1}, {"title", "  "}}, badName, 30000);
			check("spec_op copy (same points) + rename + move to the top keeps the active tree; an empty name is refused",
			      listOk && !okBad && child.Alive(), mv.dump().substr(0, 300));
			json ex, im;
			bool okEx = listOk && child.Call("export_tree_url", json::object(), ex, 30000);
			bool okIm = okEx && child.Call("import_tree_url", json{{"url", ex.value("url", "")}, {"title", "bridge link"}}, im, 60000);
			bool linkOk = okIm && im["specs"].size() == n0 + 2 && im.value("activeSpec", 0) == (int)n0 + 2 &&
			              im["specs"][n0 + 1].value("points", -1) > 0 && im["specs"][n0 + 1].value("points", -1) <= pts0 &&
			              im["specs"][n0 + 1].value("title", "") == "bridge link";
			json badLink;
			bool okBadLink = child.Call("import_tree_url", json{{"url", "https://www.pathofexile.com/passive-skill-tree/3.25.0/notatree"}, {"title", "x"}}, badLink, 60000);
			check("export_tree_url gives POB's link and import_tree_url makes a new, active tree (a link carries no cluster-jewel nodes, so at most the same points); a broken link is refused",
			      linkOk && ex.value("url", "").rfind("https://", 0) == 0 && !okBadLink && child.Alive(),
			      "pts0=" + std::to_string(pts0) + " n0=" + std::to_string(n0) + " act=" + std::to_string(im.value("activeSpec", 0)) + " specs=" + (okIm ? [&] { std::string o; for (auto& s : im["specs"]) o += s.value("title", "?") + ":" + std::to_string(s.value("points", -1)) + " "; return o; }() : im.dump().substr(0, 200)) + " bad=" + std::to_string(okBadLink));
			json rs, rsLs;
			bool okRs = linkOk && child.Call("reset_tree", json::object(), rs, 60000) && child.Call("list_specs", json::object(), rsLs, 30000);
			check("reset_tree (POB's Reset Tree button) empties only the active tree",
			      okRs && rsLs["specs"][n0 + 1].value("points", -1) == 0 && rsLs["specs"][0].value("points", -1) == pts0,
			      okRs ? rsLs.dump().substr(0, 200) : rs.dump().substr(0, 200));
			const std::string ver = ls0.value("treeVersion", "");
			json cv;
			bool okCv = okRs && child.Call("convert_tree", json{{"version", ver}, {"copy", true}}, cv, 120000);
			check("convert_tree{copy} adds a converted copy after the active tree and makes it active",
			      okCv && cv["specs"].size() == n0 + 3 && cv.value("activeSpec", 0) == (int)n0 + 3, cv.dump().substr(0, 200));
			// back to the start: drop the three trees, the original active again
			json d1, d2, d3, back, ls9;
			bool okBack = okCv && child.Call("spec_op", json{{"op", "delete"}, {"index", (int)n0 + 3}}, d1, 60000) &&
			              child.Call("spec_op", json{{"op", "delete"}, {"index", (int)n0 + 2}}, d2, 60000) &&
			              child.Call("spec_op", json{{"op", "delete"}, {"index", 1}}, d3, 60000) &&
			              child.Call("set_active_spec", json{{"index", act0}}, back, 60000) && child.Call("list_specs", json::object(), ls9, 30000);
			json stBack;
			child.Call("get_build_info", json::object(), stBack, 30000);
			check("spec_op delete (POB's confirm answered) and set_active_spec restore the original tree list",
			      okBack && ls9["specs"].size() == n0 && ls9.value("activeSpec", 0) == act0 && ls9["specs"][act0 - 1].value("points", -1) == pts0,
			      okBack ? ls9.dump().substr(0, 200) : (d1.dump() + d2.dump() + d3.dump() + back.dump()).substr(0, 300));
		}

		// --- 2a: build header, level, share code, save-as, import ---------------
		{
			json hdr;
			bool okH = okLoad && child.Call("get_build_header", json::object(), hdr, 30000);
			check("get_build_header: level and POB's own main-skill selectors",
			      okH && hdr.value("level", 0) > 0 && hdr.contains("mainSocketGroup") && hdr["mainSocketGroup"]["list"].size() > 0 &&
			          hdr["mainSocketGroup"]["list"][0].contains("labelZh"),
			      okH ? "level=" + std::to_string(hdr.value("level", 0)) + " groups=" + std::to_string(hdr["mainSocketGroup"]["list"].size()) +
			                (hdr.contains("mainSkill") ? " skills=" + std::to_string(hdr["mainSkill"]["list"].size()) : "")
			          : hdr.dump().substr(0, 300));
			const int lv0 = hdr.value("level", 0);
			json st0;
			child.Call("get_stats", json::object(), st0, 30000);
			const int lv1 = lv0 > 50 ? lv0 - 40 : lv0 + 40;
			json r1, st1;
			bool ok1 = okH && child.Call("set_build_field", json{{"field", "level"}, {"value", lv1}}, r1, 60000) &&
			           child.Call("get_stats", json::object(), st1, 30000);
			// Life alone is not enough of a witness (a CI build sits at 1 whatever
			// the level); any of the three moving proves the recalculation ran.
			auto moved = [&](const char* k) {
				return st0.contains("stats") && st1.contains("stats") && st0["stats"].value(k, 0.0) != st1["stats"].value(k, 0.0);
			};
			check("set_build_field{level} recalculates (Life/ES/TotalDPS change) and marks the build unsaved",
			      ok1 && r1.value("unsaved", false) && (moved("Life") || moved("EnergyShield") || moved("TotalDPS")),
			      ok1 ? "dps " + st0["stats"].value("TotalDPS", json()).dump() + " -> " + st1["stats"].value("TotalDPS", json()).dump() : r1.dump().substr(0, 200));
			json r2, st2;
			bool ok2 = ok1 && child.Call("set_build_field", json{{"field", "level"}, {"value", lv0}}, r2, 60000) &&
			           child.Call("get_stats", json::object(), st2, 30000);
			check("restoring the level restores Life and TotalDPS exactly",
			      ok2 && st0["stats"].value("Life", 0.0) == st2["stats"].value("Life", 1.0) &&
			          st0["stats"].value("TotalDPS", 0.0) == st2["stats"].value("TotalDPS", 1.0));

			// share code: export -> decode preview -> import back into this build
			json ex, dec;
			bool okEx = okLoad && child.Call("export_code", json::object(), ex, 60000);
			bool okDec = okEx && child.Call("decode_code", json{{"code", ex.value("code", "")}}, dec, 60000);
			auto hasSection = [&](const char* n) {
				if (!okDec || !dec.contains("sections")) return false;
				for (auto& x : dec["sections"]) if (x.get<std::string>() == n) return true;
				return false;
			};
			check("export_code -> decode_code: a code that decodes to a build with tree, items and skills",
			      okEx && ex.value("bytes", 0) > 1000 && okDec && hasSection("Build") && hasSection("Tree") && hasSection("Items") &&
			          hasSection("Skills") && dec.value("level", 0) == lv0 && !dec.value("className", "").empty(),
			      okDec ? "bytes=" + std::to_string(ex.value("bytes", 0)) + " items=" + std::to_string(dec.value("itemCount", 0)) +
			                  " skills=" + std::to_string(dec.value("skillCount", 0)) + " class=" + dec.value("className", "")
			            : (okEx ? dec.dump().substr(0, 300) : ex.dump().substr(0, 300)));
			json badDec;
			bool okBadDec = child.Call("decode_code", json{{"code", "this is not a code"}}, badDec, 60000);
			check("decode_code rejects garbage with an error instead of a crash", !okBadDec && child.Alive(), badDec.dump().substr(0, 200));
			json imp, st3;
			bool okImp = okEx && child.Call("import_code", json{{"code", ex.value("code", "")}, {"mode", "replace"}}, imp, 180000) &&
			             child.Call("get_stats", json::object(), st3, 30000);
			check("import_code{replace} of the build's own code gives the same Life and TotalDPS",
			      okImp && st0["stats"].value("Life", 0.0) == st3["stats"].value("Life", 1.0) &&
			          st0["stats"].value("TotalDPS", 0.0) == st3["stats"].value("TotalDPS", 1.0),
			      okImp ? "dps " + st0["stats"].value("TotalDPS", json()).dump() + " -> " + st3["stats"].value("TotalDPS", json()).dump()
			            : imp.dump().substr(0, 300));

			// Account import: ImportTab's OAuth / account-name state as the page
			// polls it. Offline checks first, then one real round trip against
			// pathofexile.com with an account that cannot exist (the same network
			// POB's own update check already used).
			{
				json is;
				bool okIs = child.Call("import_status", json::object(), is, 30000);
				check("import_status: not authorized, three realms, account-name flow idle, nothing imported",
				      okIs && !is.value("authorized", true) && is["realms"].size() == 3 && is["site"].value("mode", "") == "GETACCOUNTNAME" &&
				          is.value("imported", -1) == 0 && is["oauth"].contains("now"),
				      okIs ? is.dump().substr(0, 300) : is.dump().substr(0, 300));
				json bad;
				bool okBad = child.Call("fetch_characters", json{{"source", "site"}, {"realm", "PC"}, {"accountName", "nodiscriminator"}}, bad, 30000);
				check("fetch_characters{site} without the #1234 discriminator is refused (POB's Start-button rule)", !okBad && child.Alive(), bad.dump().substr(0, 200));
				json early;
				bool okEarly = child.Call("import_account_character", json{{"source", "site"}, {"realm", "PC"}, {"name", "x"}, {"what", "tree"}}, early, 30000);
				check("import_account_character{site} before a character list is refused", !okEarly && child.Alive(), early.dump().substr(0, 200));
				json fc;
				bool okFc = child.Call("fetch_characters", json{{"source", "site"}, {"realm", "PC"}, {"accountName", "pobtools-no-such-account-zz#0000"}}, fc, 30000);
				std::string mode = "";
				json last;
				for (int i = 0; okFc && i < 60; i++) {
					Sleep(500);
					if (!child.Call("import_status", json::object(), last, 30000)) break;
					mode = last["site"].value("mode", "");
					if (mode != "DOWNLOADCHARLIST") break;
				}
				std::string status = last.is_object() ? last["site"].value("status", "") : "";
				check("fetch_characters{site}: POB's own download ran in the subscript and its error text came back (mode returns to GETACCOUNTNAME)",
				      okFc && fc.value("started", false) && mode == "GETACCOUNTNAME" && status.find("^") == 0 && status != "Idle",
				      "mode=" + mode + " status=" + status.substr(0, 120));
			}

			// Build list management: New (an unnamed build), New Folder, Rename,
			// Copy, Delete, all under the sandbox's build folder; then the sample
			// build again so the checks below start from the same place.
			{
				json nb;
				bool okNb = child.Call("new_build", json{{"name", "bridge_new"}}, nb, 120000);
				json nbi;
				bool okNbi = okNb && child.Call("get_build_info", json::object(), nbi, 30000);
				check("new_build: an unnamed, file-less build with no points spent",
				      okNb && nb.value("buildName", "") == "bridge_new" && !nb.contains("dbFileName") && okNbi && nbi["points"].value("used", -1) == 0,
				      okNb ? nb.dump().substr(0, 200) + " " + (okNbi ? nbi["points"].dump() : "") : nb.dump().substr(0, 300));
				json nf;
				bool okNf = child.Call("new_folder", json{{"subPath", ""}, {"name", "bridge_dir"}}, nf, 30000);
				check("new_folder creates a folder under the build folder", okNf && GetFileAttributesW((sandbox + L"\\Builds\\bridge_dir").c_str()) != INVALID_FILE_ATTRIBUTES, nf.dump().substr(0, 200));
				json badNf;
				bool okBadNf = child.Call("new_folder", json{{"subPath", ""}, {"name", "../escape"}}, badNf, 30000);
				check("new_folder refuses path separators in the name", !okBadNf && child.Alive(), badNf.dump().substr(0, 200));
				const std::wstring nbPath = sandbox + L"\\Builds\\bridge_new.xml";
				DeleteFileW(nbPath.c_str());
				json svn;
				bool okSvn = child.Call("save_build_as", json{{"path", narrow(nbPath)}}, svn, 60000);
				json rn;
				bool okRn = okSvn && child.Call("rename_build", json{{"path", narrow(nbPath)}, {"subPath", ""}, {"isFolder", false}, {"newName", "bridge_renamed"}}, rn, 30000);
				const std::wstring rnPath = sandbox + L"\\Builds\\bridge_renamed.xml";
				json cp;
				bool okCp = okRn && child.Call("rename_build", json{{"path", narrow(rnPath)}, {"subPath", ""}, {"isFolder", false}, {"newName", "bridge_copy"}, {"copy", true}}, cp, 30000);
				const std::wstring cpPath = sandbox + L"\\Builds\\bridge_copy.xml";
				json hdr;
				child.Call("get_build_header", json::object(), hdr, 30000);
				check("rename_build moves the saved file (the open build follows it) and copy=true duplicates it",
				      okRn && okCp && GetFileAttributesW(nbPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
				          GetFileAttributesW(rnPath.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(cpPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
				          hdr.value("buildName", "") == "bridge_renamed",
				      "rn=" + rn.dump().substr(0, 100) + " cp=" + cp.dump().substr(0, 100) + " name=" + hdr.value("buildName", ""));
				// the list screen's search box and sort drop-down
				json lbf;
				bool okLbf = child.Call("list_builds", json{{"subPath", ""}, {"filter", "bridge_copy"}, {"sortMode", "LEVEL"}}, lbf, 30000);
				bool onlyCopy = okLbf && lbf["entries"].size() >= 1;
				if (okLbf) for (auto& e : lbf["entries"]) if (e.value("buildName", "") != "bridge_copy") onlyCopy = false;
				json lbs;
				child.Call("list_builds", json{{"subPath", ""}, {"filter", ""}, {"sortMode", "NAME"}}, lbs, 30000);
				check("list_builds{filter, sortMode}: POB's search keeps only the match, the sort mode sticks and its choices are listed",
				      onlyCopy && lbf.value("sortMode", "") == "LEVEL" && lbf["sortModes"].size() >= 4 && lbs.value("sortMode", "") == "NAME",
				      lbf.dump().substr(0, 300));
				// cut + paste into a folder, copy + paste back, and a folder into itself
				const std::wstring movedPath = sandbox + L"\\Builds\\bridge_dir\\bridge_copy.xml";
				json mv, mvBack, mvSelf;
				bool okMv = child.Call("move_build", json{{"path", narrow(cpPath)}, {"subPath", ""}, {"isFolder", false}, {"name", "bridge_copy.xml"}, {"targetSubPath", "bridge_dir/"}}, mv, 30000);
				bool okMvBack = okMv && child.Call("move_build", json{{"path", narrow(movedPath)}, {"subPath", "bridge_dir/"}, {"isFolder", false}, {"name", "bridge_copy.xml"}, {"targetSubPath", ""}, {"copy", true}}, mvBack, 30000);
				bool okMvSelf = child.Call("move_build", json{{"path", narrow(sandbox + L"\\Builds\\bridge_dir")}, {"subPath", ""}, {"isFolder", true}, {"name", "bridge_dir"}, {"targetSubPath", "bridge_dir/"}}, mvSelf, 30000);
				check("move_build moves a build into a folder, copy=true copies it back, and a folder cannot go into itself",
				      okMv && okMvBack && GetFileAttributesW(movedPath.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(cpPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
				          !okMvSelf && child.Alive(),
				      mv.dump().substr(0, 120) + " " + mvBack.dump().substr(0, 120) + " " + mvSelf.dump().substr(0, 120));
				// the moved copy is deleted before its folder: POB's recursive
				// RemoveDir does not delete a non-empty folder under Wine
				json delMoved;
				child.Call("delete_build", json{{"path", narrow(movedPath)}, {"isFolder", false}}, delMoved, 30000);
				json delOpen;
				bool okDelOpen = child.Call("delete_build", json{{"path", narrow(rnPath)}, {"isFolder", false}}, delOpen, 30000);
				check("delete_build refuses the build that is open", !okDelOpen && child.Alive(), delOpen.dump().substr(0, 200));
				json delCp, delDir, delOut;
				bool okDelCp = child.Call("delete_build", json{{"path", narrow(cpPath)}, {"isFolder", false}}, delCp, 30000);
				bool okDelDir = child.Call("delete_build", json{{"path", narrow(sandbox + L"\\Builds\\bridge_dir")}, {"isFolder", true}, {"recursive", true}}, delDir, 30000);
				bool okDelOut = child.Call("delete_build", json{{"path", narrow(sandbox + L"\\Launch.lua")}, {"isFolder", false}}, delOut, 30000);
				check("delete_build removes the copy and the folder (recursive), and refuses anything outside the build folder",
				      okDelCp && okDelDir && GetFileAttributesW(cpPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
				          GetFileAttributesW((sandbox + L"\\Builds\\bridge_dir").c_str()) == INVALID_FILE_ATTRIBUTES && !okDelOut &&
				          GetFileAttributesW((sandbox + L"\\Launch.lua").c_str()) != INVALID_FILE_ATTRIBUTES,
				      delCp.dump().substr(0, 80) + " " + delDir.dump().substr(0, 80) + " " + delOut.dump().substr(0, 120));
				// back to the sample build for everything that follows
				json rl;
				bool okRl = child.Call("load_build_file", json{{"path", narrow(sandbox + L"\\Builds\\" + sample)}}, rl, 120000);
				DeleteFileW(rnPath.c_str());
				check("load_build_file after the new-build round trip reopens the sample", okRl && rl.value("buildName", "") != "bridge_renamed", rl.dump().substr(0, 200));
			}

			// save as: under the sandbox's build folder, then reload it and run the oracle again
			const std::wstring savePath = sandbox + L"\\Builds\\bridge_saveas.xml";
			DeleteFileW(savePath.c_str());
			json sv;
			bool okSv = okLoad && child.Call("save_build_as", json{{"path", narrow(savePath)}}, sv, 60000);
			check("save_build_as writes the file under the build folder and clears unsaved",
			      okSv && GetFileAttributesW(savePath.c_str()) != INVALID_FILE_ATTRIBUTES && !sv.value("unsaved", true) &&
			          sv.value("buildName", "") == "bridge_saveas",
			      okSv ? sv.dump().substr(0, 200) : sv.dump().substr(0, 300));
			json ld, st4;
			bool okLd = okSv && child.Call("load_build_file", json{{"path", narrow(savePath)}}, ld, 120000) &&
			            child.Call("get_stats", json::object(), st4, 30000);
			if (okLd) {
				auto oracle = ParsePlayerStats(ReadFileA(savePath));
				int missing = 0;
				std::vector<std::string> ex2;
				int bad = CompareStats(oracle, st4["stats"], 1e-9, missing, ex2);
				std::string detail = "checked=" + std::to_string(oracle.size()) + " bad=" + std::to_string(bad) + " missing=" + std::to_string(missing);
				for (auto& e : ex2) detail += "; " + e;
				check("the file save_build_as wrote reloads and its <PlayerStat> equal get_stats (rel 1e-9)",
				      oracle.size() > 20 && bad == 0 && missing == 0, detail);
			} else {
				check("the file save_build_as wrote reloads and its <PlayerStat> equal get_stats (rel 1e-9)", false, ld.dump().substr(0, 300));
			}
			json outside;
			bool okOut = child.Call("save_build_as", json{{"path", narrow(exeDir + L"PobTools\\outside.xml")}}, outside, 30000);
			check("save_build_as refuses a path outside POB's build folder", !okOut && GetFileAttributesW((exeDir + L"PobTools\\outside.xml").c_str()) == INVALID_FILE_ATTRIBUTES,
			      outside.dump().substr(0, 200));
			json sv2;
			bool okSv2 = okLd && child.Call("save_build", json::object(), sv2, 60000);
			check("save_build (in place) succeeds on a build that has a file", okSv2 && !sv2.value("unsaved", true));
		}

		// --- 2b: items -----------------------------------------------------------
		{
			json li;
			bool okLi = okLoad && child.Call("list_items", json::object(), li, 60000);
			int shownSlots = 0, withValid = 0;
			if (okLi) for (auto& sl : li["slots"]) { if (sl.value("shown", false)) shownSlots++; if (sl.contains("valid")) withValid++; }
			check("list_items: the build's items, POB's slot grid (with validity) and item sets",
			      okLi && li["items"].size() > 5 && shownSlots >= 15 && withValid == (int)li["slots"].size() && li["itemSets"].size() >= 1 &&
			          li["items"][0].contains("nameZh"),
			      okLi ? "items=" + std::to_string(li["items"].size()) + " slots=" + std::to_string(li["slots"].size()) + " shown=" + std::to_string(shownSlots)
			           : li.dump().substr(0, 300));
			// The first few items: every tooltip has lines and a header, and at
			// least one line somewhere came back translated (a jewel's text may
			// have no dictionary entry, so no single item is required to).
			int ttOk = 0, ttTried = 0, zhLines = 0;
			for (size_t i = 0; okLi && i < li["items"].size() && i < 5; i++) {
				json tt;
				ttTried++;
				if (!child.Call("item_tooltip", json{{"id", li["items"][i].value("id", 0LL)}}, tt, 60000)) continue;
				int textLines = 0;
				// The injector already renames items in POB's own tables, so "raw"
				// can be Chinese too; count lines that carry any non-ASCII text.
				for (auto& l : tt["lines"]) if (l.contains("text")) {
					textLines++;
					const std::string tx = l.value("text", "");
					for (unsigned char c : tx) if (c >= 0x80) { zhLines++; break; }
				}
				if (textLines >= 3 && !tt.value("header", "").empty()) ttOk++;
			}
			check("item_tooltip: POB's own item tooltip for each item, with translated text",
			      ttTried > 0 && ttOk == ttTried && zhLines >= 1, "items=" + std::to_string(ttTried) + " ok=" + std::to_string(ttOk) + " translatedLines=" + std::to_string(zhLines));

			// add a unique ring (equip=false), swap it into Ring 1, put the old one back, delete it
			json st0;
			child.Call("get_stats", json::object(), st0, 30000);
			const std::string ringRaw =
			    "Rarity: UNIQUE\nShavronne's Revelation\nMoonstone Ring\nItem Level: 80\nImplicits: 1\n+60 to Intelligence\n"
			    "Right ring slot: You cannot Regenerate Mana\nRight ring slot: Regenerate 6% of Energy Shield per second\n"
			    "Right ring slot: +265 to maximum Mana\nLeft ring slot: You cannot Recharge or Regenerate Energy Shield\n"
			    "Left ring slot: Regenerate 42.4 Mana per Second\nLeft ring slot: +250 to maximum Energy Shield\n";
			json added;
			bool okAdd = okLi && child.Call("add_item", json{{"raw", ringRaw}, {"equip", false}}, added, 60000);
			long long newId = okAdd ? added["item"].value("id", 0LL) : 0;
			json li2;
			bool okLi2 = okAdd && child.Call("list_items", json::object(), li2, 60000);
			check("add_item parses pasted text through new(\"Item\") and AddItem (count +1, not equipped)",
			      okAdd && newId > 0 && added["item"].value("rarity", "") == "UNIQUE" && added["item"].value("primarySlot", "") == "Ring 1" &&
			          okLi2 && li2["items"].size() == li["items"].size() + 1,
			      okAdd ? added.dump().substr(0, 200) : added.dump().substr(0, 300));
			long long prevRing = 0;
			if (okLi2) for (auto& sl : li2["slots"]) if (sl.value("name", "") == "Ring 1") prevRing = sl.value("selItemId", 0LL);
			json eq, st1;
			bool okEq = okLi2 && child.Call("equip_item", json{{"id", newId}, {"slotName", "Ring 1"}}, eq, 60000) &&
			            child.Call("get_stats", json::object(), st1, 30000);
			check("equip_item into Ring 1 changes the calculation",
			      okEq && st0.contains("stats") && st1.contains("stats") &&
			          (st0["stats"].value("Mana", 0.0) != st1["stats"].value("Mana", 0.0) || st0["stats"].value("TotalDPS", 0.0) != st1["stats"].value("TotalDPS", 0.0)),
			      okEq ? "mana " + st0["stats"].value("Mana", json()).dump() + " -> " + st1["stats"].value("Mana", json()).dump() : eq.dump().substr(0, 300));
			json back, st2;
			bool okBack = okEq && (prevRing > 0 ? child.Call("equip_item", json{{"id", prevRing}, {"slotName", "Ring 1"}}, back, 60000)
			                                    : child.Call("unequip_slot", json{{"slotName", "Ring 1"}}, back, 60000)) &&
			              child.Call("delete_item", json{{"id", newId}}, back, 60000) && child.Call("get_stats", json::object(), st2, 30000);
			json li3;
			bool okLi3 = okBack && child.Call("list_items", json::object(), li3, 60000);
			check("putting the old ring back and deleting the new one restores count, Mana and TotalDPS",
			      okLi3 && li3["items"].size() == li["items"].size() && st0["stats"].value("Mana", 0.0) == st2["stats"].value("Mana", 1.0) &&
			          st0["stats"].value("TotalDPS", 0.0) == st2["stats"].value("TotalDPS", 1.0),
			      okLi3 ? "count=" + std::to_string(li3["items"].size()) + " mana " + st2["stats"].value("Mana", json()).dump() : back.dump().substr(0, 300));
			json badAdd;
			bool okBadAdd = child.Call("add_item", json{{"raw", "Rarity: RARE\nNonsense\nNot A Base At All\n"}}, badAdd, 60000);
			check("add_item refuses text with no known base (error, engine stays up)", !okBadAdd && child.Alive(), badAdd.dump().substr(0, 200));

			// Chinese item text (the game client's Ctrl+C) goes through the engine's
			// reverse translator before Item(): the same jewel, written the way the
			// Traditional Chinese client prints it, must resolve to the same base.
			{
				const std::string zhRaw = u8"稀有度: 魔法\n習武的 鈷藍珠寶\n鈷藍珠寶\n--------\n物品等級: 20\n--------\n增加 10% 最大生命\n";
				json zhAdd;
				bool okZh = child.Call("add_item", json{{"raw", zhRaw}, {"equip", false}}, zhAdd, 60000);
				long long zhId = okZh ? zhAdd["item"].value("id", 0LL) : 0;
				json zhRawBack;
				bool okZhRaw = okZh && zhId > 0 && child.Call("item_raw", json{{"id", zhId}}, zhRawBack, 60000);
				std::string en = okZhRaw ? zhRawBack.value("raw", "") : "";
				check("add_item: Chinese item text is reverse-translated (Cobalt Jewel base, English raw kept) and reports reversed=true",
				      okZh && zhAdd.value("reversed", false) && zhAdd["item"].value("baseName", "") == "Cobalt Jewel" && en.find("Cobalt Jewel") != std::string::npos &&
				          en.find("Rarity: MAGIC") != std::string::npos,
				      okZh ? zhAdd.dump().substr(0, 200) + " raw=" + en.substr(0, 80) : zhAdd.dump().substr(0, 300));
				if (zhId > 0) { json del; child.Call("delete_item", json{{"id", zhId}}, del, 60000); }
				json zhTip;
				bool okZhTip = child.Call("item_tooltip", json{{"raw", zhRaw}}, zhTip, 60000);
				check("item_tooltip{raw} previews Chinese item text through the same translator",
				      okZhTip && zhTip.value("reversed", false) && zhTip["lines"].size() >= 2, zhTip.dump().substr(0, 200));
			}

			json dbU, dbZh, dbR;
			bool okDbU = okLoad && child.Call("item_db", json{{"kind", "unique"}, {"query", "shavronne"}, {"page", 1}, {"size", 10}}, dbU, 180000);
			bool okDbZh = okDbU && child.Call("item_db", json{{"kind", "unique"}, {"query", u8"\u859b\u6717"}, {"page", 1}, {"size", 10}}, dbZh, 60000);
			bool okDbR = okDbU && child.Call("item_db", json{{"kind", "rare"}, {"page", 1}, {"size", 5}}, dbR, 60000);
			check("item_db: uniques searchable in English and in the translated name, rares listed, entries carry raw text",
			      okDbU && dbU.value("total", 0) >= 3 && dbU["items"][0].contains("raw") && okDbZh && dbZh.value("total", 0) >= 1 &&
			          okDbR && dbR.value("total", 0) > 50 && dbR["items"].size() && dbR["items"][0].contains("raw"),
			      "en=" + std::to_string(dbU.value("total", 0)) + " zh=" + std::to_string(dbZh.value("total", 0)) + " rares=" + std::to_string(dbR.value("total", 0)));
			json dbTt;
			bool okDbTt = okDbU && dbU["items"].size() && child.Call("item_tooltip", json{{"raw", dbU["items"][0].value("raw", "")}, {"rarity", "UNIQUE"}, {"dbMode", true}}, dbTt, 60000);
			check("item_tooltip{raw} previews a database entry without adding it", okDbTt && dbTt["lines"].size() >= 3);
		}

		// --- 2b2: item editing, comparison, list management, item databases ----
		{
			json li;
			bool okLi = okLoad && child.Call("list_items", json::object(), li, 60000);
			long long idRare = 0, idHelmet = 0, idUnused = 0, idUnusedRing = 0;
			int usedSlot = 0, unusedN = 0;
			if (okLi) for (auto& it : li["items"]) {
				const bool used = it.contains("usedIn") && !it["usedIn"].is_null();
				if (used) usedSlot++; else unusedN++;
				const std::string rarity = it.value("rarity", ""), type = it.value("type", "");
				const long long id = it.value("id", 0LL);
				if (used && !idRare && rarity == "RARE" && type != "Jewel" && type != "Flask") idRare = id;
				if (used && !idHelmet && type == "Helmet") idHelmet = id;
				if (!used && !idUnused) idUnused = id;
				if (!used && !idUnusedRing && type == "Ring") idUnusedRing = id;
			}
			check("list_items: usedIn says where each item is (slot / abyss / jewel) or is absent for unused ones",
			      okLi && usedSlot >= 10 && unusedN >= 1 && idRare > 0,
			      "used=" + std::to_string(usedSlot) + " unused=" + std::to_string(unusedN));

			// editing session: changes stay on POB's displayItem until commit
			json raw0, ed, edQ, edI;
			bool okRaw0 = idRare > 0 && child.Call("item_raw", json{{"id", idRare}}, raw0, 60000);
			bool okEd = okRaw0 && child.Call("item_edit_begin", json{{"id", idRare}}, ed, 60000);
			check("item_edit_begin{id}: SetDisplayItem on a copy (tooltip, sockets, quality, influence, mod lines, actions)",
			      okEd && !ed.value("isNew", true) && ed["tooltip"]["lines"].size() >= 3 && ed["sockets"].size() >= 1 && ed["quality"].value("shown", false) &&
			          ed["modLines"].size() >= 1 && ed["influence"]["options"].size() >= 5 && ed["actions"].value("corrupt", false),
			      okEd ? "sockets=" + std::to_string(ed["sockets"].size()) + " mods=" + std::to_string(ed["modLines"].size()) : ed.dump().substr(0, 300));
			const int q0 = okEd && ed["quality"].contains("value") && !ed["quality"]["value"].is_null() ? ed["quality"].value("value", 0) : 0;
			const int q1 = q0 == 27 ? 28 : 27;
			bool okQ = okEd && child.Call("item_edit_set", json{{"quality", q1}}, edQ, 60000);
			json raw1;
			bool okRaw1 = okQ && child.Call("item_raw", json{{"id", idRare}}, raw1, 60000);
			check("item_edit_set{quality}: the panel's EditControl callback changes the display item, the build's item is untouched",
			      okQ && edQ["quality"].value("value", -1) == q1 && okRaw1 && raw1.value("raw", "") == raw0.value("raw", "x"),
			      okQ ? "q " + std::to_string(q0) + " -> " + std::to_string(edQ["quality"].value("value", -1)) : edQ.dump().substr(0, 300));
			bool okI = okQ && child.Call("item_edit_set", json{{"influence", json::array({2, 1})}}, edI, 60000);
			check("item_edit_set{influence}: the influence dropdowns' callback (ResetInfluence + key) is applied",
			      okI && edI["influence"]["current"].size() == 1 && edI["influence"]["sel"][0] == 2,
			      okI ? edI["influence"]["current"].dump() : edI.dump().substr(0, 300));
			// the Corrupt dialog, captured: pick the first implicit and press its own Corrupt button
			json pc, pc2, pc3;
			bool okPc = okI && child.Call("item_edit_popup", json{{"kind", "corrupt"}, {"action", "open"}}, pc, 60000);
			int implicitOpts = 0;
			if (okPc) for (auto& c : pc["controls"]) if (c.value("name", "") == "implicit1") implicitOpts = (int)c["options"].size();
			bool okPc2 = okPc && implicitOpts > 1 && child.Call("item_edit_popup", json{{"action", "pick"}, {"name", "implicit1"}, {"sel", 2}}, pc2, 60000) &&
			             child.Call("item_edit_popup", json{{"action", "apply"}, {"button", "save"}}, pc3, 60000);
			check("item_edit_popup{corrupt}: the dialog's controls are read back, a pick and its Corrupt button go through the dialog's own closures",
			      okPc2 && pc3["summary"].value("corrupted", false) && pc3["tooltip"]["lines"].size() > ed["tooltip"]["lines"].size() && pc3["popup"].is_null(),
			      okPc ? "implicits=" + std::to_string(implicitOpts) + " lines " + std::to_string(ed["tooltip"]["lines"].size()) + " -> " + std::to_string(okPc2 ? pc3["tooltip"]["lines"].size() : 0) : pc.dump().substr(0, 300));
			json cancel, raw2;
			bool okCancel = okEd && child.Call("item_edit_cancel", json::object(), cancel, 60000) && child.Call("item_raw", json{{"id", idRare}}, raw2, 60000);
			json edNone;
			bool okNone = child.Call("item_edit_state", json::object(), edNone, 60000);
			check("item_edit_cancel drops the display item; the build's item is still what it was; no session afterwards",
			      okCancel && raw2.value("raw", "") == raw0.value("raw", "x") && !okNone && child.Alive(), edNone.dump().substr(0, 120));

			// Every mod line POB draws, not just the explicit ones. A unique with an
			// implicit and rolled ranges is the case that caught this: the panel
			// listed four of its five lines (the missing one being the implicit,
			// the line the tooltip prints above the separator), and every range row
			// came back blank because the drop-down's list is plain strings while
			// the data lives on the item.
			json edU;
			const char* kUniqueRaw =
			    "Rarity: UNIQUE\nGoldrim\nLeather Cap\nImplicits: 1\n{range:0.5}+(10-20) to Evasion Rating\n"
			    "{range:0.5}+(30-40)% to all Elemental Resistances\n";
			bool okU = child.Call("item_edit_begin", json{{"raw", kUniqueRaw}}, edU, 60000);
			int implicitLines = 0, explicitLines = 0, labelledRanges = 0;
			if (okU) {
				for (auto& m : edU["modLines"]) {
					const std::string sec = m.value("section", "");
					if (sec == "implicit") implicitLines++;
					else if (sec == "explicit") explicitLines++;
				}
				for (auto& r : edU["ranges"])
					if (!r.value("label", "").empty() && r.contains("range") && !r["range"].is_null()) labelledRanges++;
			}
			check("item_edit_state lists the implicit lines too, tagged by section, and every range row carries its text and roll",
			      okU && implicitLines >= 1 && explicitLines >= 1 && labelledRanges == (int)edU["ranges"].size() &&
			          edU["ranges"].size() >= 2,
			      okU ? "implicit=" + std::to_string(implicitLines) + " explicit=" + std::to_string(explicitLines) +
			                " ranges=" + std::to_string(edU["ranges"].size()) + " labelled=" + std::to_string(labelledRanges)
			          : edU.dump().substr(0, 300));
			// the roll of the FIRST line (the implicit) has to move: with the index
			// space still starting at the explicits, this wrote to the wrong line
			json edU2;
			const double roll0 = (okU && !edU["ranges"].empty()) ? edU["ranges"][0].value("range", -1.0) : -1.0;
			bool okU2 = okU && child.Call("item_edit_set", json{{"range", json{{"index", 1}, {"value", 1.0}}}}, edU2, 60000);
			const double roll1 = okU2 && !edU2["ranges"].empty() ? edU2["ranges"][0].value("range", -1.0) : -1.0;
			check("item_edit_set{range} moves the line it names (the implicit is line 1, not the first explicit)",
			      okU2 && roll0 >= 0.0 && roll1 > roll0,
			      "roll " + std::to_string(roll0) + " -> " + std::to_string(roll1));
			json edUc;
			child.Call("item_edit_cancel", json::object(), edUc, 60000);



			// commit on the same id = POB's "Save": the build's item changes, then put it back
			json edS, edS2, cm, raw3, cmBack;
			bool okSave = okRaw0 && child.Call("item_edit_begin", json{{"id", idRare}}, edS, 60000) && child.Call("item_edit_set", json{{"quality", q1}}, edS2, 60000) &&
			              child.Call("item_edit_commit", json{{"equip", true}}, cm, 60000) && child.Call("item_raw", json{{"id", idRare}}, raw3, 60000);
			bool okRestore = okSave && child.Call("item_edit_begin", json{{"id", idRare}}, edS, 60000) && child.Call("item_edit_set", json{{"quality", q0}}, edS2, 60000) &&
			                 child.Call("item_edit_commit", json{{"equip", true}}, cmBack, 60000) && child.Call("item_raw", json{{"id", idRare}}, raw2, 60000);
			check("item_edit_commit on an existing id = Save (AddDisplayItem replaces it, raw changes), restoring puts the raw back",
			      okSave && !cm.value("added", true) && cm.value("id", 0LL) == idRare && raw3.value("raw", "") != raw0.value("raw", "") &&
			          raw3.value("raw", "").find("Quality: " + std::to_string(q1)) != std::string::npos && okRestore && raw2.value("raw", "") == raw0.value("raw", "x"),
			      okSave ? "quality line " + (raw3.value("raw", "").find("Quality") != std::string::npos ? std::string("changed") : std::string("missing")) : cm.dump().substr(0, 300));

			// craft a new rare from the Craft Item dialog, pick a prefix, add it to the build, delete it again
			json co, edC, edA, cmC, liC, stC, delC;
			bool okCo = okLoad && child.Call("craft_item_options", json::object(), co, 60000);
			std::string craftType, craftBase;
			if (okCo && co["types"].size()) { craftType = co["types"][0].value("type", ""); if (co["types"][0]["bases"].size()) craftBase = co["types"][0]["bases"][0].value("name", ""); }
			check("craft_item_options: the Craft Item dialog's rarities plus POB's base types and bases",
			      okCo && co["rarities"].size() >= 4 && co["types"].size() >= 40 && !craftBase.empty(),
			      okCo ? "types=" + std::to_string(co["types"].size()) + " first=" + craftType + "/" + craftBase : co.dump().substr(0, 300));
			bool okEdC = okCo && child.Call("item_edit_begin", json{{"craft", json{{"rarity", "RARE"}, {"type", craftType}, {"base", craftBase}}}}, edC, 60000);
			bool okEdA = okEdC && edC["affixes"].size() >= 1 && child.Call("item_edit_affix", json{{"index", 1}, {"sel", 2}}, edA, 60000);
			// count the EXPLICIT lines: modLines carries every list POB draws, so a
			// base with an implicit of its own would otherwise fail this
			int craftExplicit = 0;
			if (okEdA) for (auto& m : edA["modLines"]) if (m.value("section", "") == "explicit") craftExplicit++;
			check("item_edit_begin{craft}: Create through the dialog gives a new crafted item with affix slots; item_edit_affix picks a prefix through the dropdown's selFunc",
			      okEdA && edC.value("isNew", false) && edC.value("crafted", false) && craftExplicit == 1 && edA["affixes"][0].value("sel", 1) == 2,
			      okEdC ? "affixSlots=" + std::to_string(edC["affixes"].size()) + " explicit after=" + std::to_string(craftExplicit) +
			                  " total=" + std::to_string(okEdA ? edA["modLines"].size() : 0)
			            : edC.dump().substr(0, 300));
			bool okCmC = okEdA && child.Call("item_edit_commit", json{{"equip", false}}, cmC, 60000) && child.Call("list_items", json::object(), liC, 60000) &&
			             child.Call("get_stats", json::object(), stC, 30000);
			long long craftedId = okCmC ? cmC.value("id", 0LL) : 0;
			check("item_edit_commit on a new item = Add to build (count +1, not equipped, stats still computable)",
			      okCmC && cmC.value("added", false) && craftedId > 0 && liC["items"].size() == li["items"].size() + 1 && stC.contains("stats"),
			      okCmC ? "id=" + std::to_string(craftedId) : cmC.dump().substr(0, 300));
			if (craftedId > 0) child.Call("delete_item", json{{"id", craftedId}}, delC, 60000);

			// the Enchant dialog on a helmet
			if (idHelmet > 0) {
				json edH, pe, cancelH;
				bool okPe = child.Call("item_edit_begin", json{{"id", idHelmet}}, edH, 60000) && edH["actions"].value("enchant", false) &&
				            child.Call("item_edit_popup", json{{"kind", "enchant"}, {"action", "open"}}, pe, 60000);
				int enchOpts = 0;
				if (okPe) for (auto& c : pe["controls"]) if (c.value("name", "") == "enchantment") enchOpts = (int)c["options"].size();
				check("item_edit_popup{enchant}: the helmet's enchantment list comes from the dialog", okPe && enchOpts >= 1, okPe ? "options=" + std::to_string(enchOpts) : pe.dump().substr(0, 300));
				child.Call("item_edit_cancel", json::object(), cancelH, 60000);
			}

			// the Anoint dialog on an amulet: the notable list plus what a notable does
			{
				long long idAmulet = 0;
				if (okLi) for (auto& it : li["items"]) if (it.value("type", "") == "Amulet") { idAmulet = it.value("id", 0LL); break; }
				json edA2, pa, tip, cancelA;
				bool okPa = idAmulet > 0 && child.Call("item_edit_begin", json{{"id", idAmulet}}, edA2, 60000) && edA2["actions"].value("anoint", false) &&
				            child.Call("item_edit_popup", json{{"kind", "anoint"}, {"action", "open"}}, pa, 120000);
				long long firstNode = 0;
				int nodeOpts = 0;
				if (okPa) for (auto& c : pa["controls"]) if (c.value("name", "") == "notableDB") {
					nodeOpts = (int)c["options"].size();
					if (nodeOpts) firstNode = c["options"][0].value("id", 0LL);
				}
				bool okTip = firstNode > 0 && child.Call("item_edit_popup", json{{"action", "tip"}, {"name", "notableDB"}, {"value", firstNode}}, tip, 120000);
				int tipLines = okTip ? (int)tip["tooltip"].size() : 0;
				bool hasCompare = false;
				if (okTip) for (auto& l : tip["tooltip"]) if (l.contains("text") && l.value("text", "").find("^x") != std::string::npos) hasCompare = true;
				check("item_edit_popup{anoint}: the notable list, and a node's own effect plus AppendAnointTooltip's comparison",
				      okPa && nodeOpts > 100 && okTip && tipLines >= 3 && hasCompare,
				      okPa ? "nodes=" + std::to_string(nodeOpts) + " tipLines=" + std::to_string(tipLines) : pa.dump().substr(0, 300));
				child.Call("item_edit_cancel", json::object(), cancelA, 60000);
			}

			// comparison: the list tooltip of an unused item carries the stat-difference block
			if (idUnused > 0) {
				json ttC, ttN;
				bool okTt = child.Call("item_tooltip", json{{"id", idUnused}}, ttC, 60000) && child.Call("item_tooltip", json{{"id", idUnused}, {"compare", false}}, ttN, 60000);
				check("item_tooltip compares against POB's comparison slot by default (more lines than compare=false)",
				      okTt && ttC["lines"].size() > ttN["lines"].size(),
				      okTt ? std::to_string(ttN["lines"].size()) + " -> " + std::to_string(ttC["lines"].size()) : ttC.dump().substr(0, 200));
			}

			// Ctrl+Click: equip into the primary slot, click again to take it out, then restore the slot
			if (idUnusedRing > 0) {
				json ep1, ep2, liE, back;
				bool okEp = child.Call("equip_primary", json{{"id", idUnusedRing}}, ep1, 60000) && child.Call("list_items", json::object(), liE, 60000);
				std::string slotName = okEp ? ep1.value("slotName", "") : "";
				long long prev = 0;
				if (okLi) for (auto& sl : li["slots"]) if (sl.value("name", "") == slotName) prev = sl.value("selItemId", 0LL);
				bool inSlot = false;
				if (okEp) for (auto& sl : liE["slots"]) if (sl.value("name", "") == slotName && sl.value("selItemId", 0LL) == idUnusedRing) inSlot = true;
				bool okEp2 = okEp && child.Call("equip_primary", json{{"id", idUnusedRing}}, ep2, 60000);
				if (prev > 0) child.Call("equip_item", json{{"id", prev}, {"slotName", slotName}}, back, 60000);
				check("equip_primary toggles the item in its primary slot (ItemListControl's Ctrl+Click)",
				      okEp2 && ep1.value("equipped", false) && inSlot && !ep2.value("equipped", true), okEp ? "slot=" + slotName : ep1.dump().substr(0, 200));
			}

			// list management: Sort, then Del Unused (the sandbox's unused items go)
			json so, liS, du, liD;
			bool okSo = okLi && child.Call("sort_items", json::object(), so, 60000) && child.Call("list_items", json::object(), liS, 60000);
			bool sortedOk = okSo && liS["items"].size() == li["items"].size();
			bool okDu = okSo && child.Call("delete_unused_items", json::object(), du, 60000) && child.Call("list_items", json::object(), liD, 60000);
			int stillUnused = 0;
			if (okDu) for (auto& it : liD["items"]) if (!it.contains("usedIn") || it["usedIn"].is_null()) stillUnused++;
			check("sort_items keeps every item; delete_unused_items removes exactly the unused ones (Del Unused's own rule)",
			      sortedOk && okDu && du.value("deleted", -1) == unusedN && stillUnused == 0 && liD["items"].size() == li["items"].size() - unusedN,
			      okDu ? "deleted=" + std::to_string(du.value("deleted", -1)) + " expected=" + std::to_string(unusedN) : du.dump().substr(0, 300));

			// item databases through ItemDBControl's own filters and sort
			json dbo, dbHelm, dbAll, dbStat, dbReq, dbRare;
			bool okDbo = okLoad && child.Call("item_db_options", json{{"kind", "unique"}}, dbo, 180000);
			check("item_db_options: ItemDBControl's slot/type/league/requirement/obtainable/search-mode/sort lists",
			      okDbo && dbo["slot"].size() >= 10 && dbo["type"].size() >= 6 && dbo["league"].size() >= 5 && dbo["requirement"].size() == 4 && dbo["obtainable"].size() >= 7 &&
			          dbo["searchMode"].size() == 3 && dbo["sort"].size() >= 3,
			      okDbo ? "slots=" + std::to_string(dbo["slot"].size()) + " leagues=" + std::to_string(dbo["league"].size()) + " sorts=" + std::to_string(dbo["sort"].size()) : dbo.dump().substr(0, 300));
			int helmIdx = 0;
			if (okDbo) for (size_t i = 0; i < dbo["slot"].size(); i++) if (dbo["slot"][i].value("label", "") == "Helmet") helmIdx = (int)i + 1;
			bool okHelm = helmIdx > 0 && child.Call("item_db", json{{"kind", "unique"}, {"slot", helmIdx}, {"obtainable", 2}, {"page", 1}, {"size", 5}}, dbHelm, 120000) &&
			              child.Call("item_db", json{{"kind", "unique"}, {"obtainable", 2}, {"page", 1}, {"size", 5}}, dbAll, 120000);
			bool allHelm = okHelm && dbHelm["items"].size() == 5;
			if (okHelm) for (auto& it : dbHelm["items"]) if (it.value("primarySlot", "") != "Helmet") allHelm = false;
			check("item_db{slot}: DoesItemMatchFilters narrows to the slot (every hit's primary slot is Helmet, fewer than all)",
			      allHelm && dbHelm.value("total", 0) > 50 && dbHelm.value("total", 0) < dbAll.value("total", 0),
			      okHelm ? "helmets=" + std::to_string(dbHelm.value("total", 0)) + " all=" + std::to_string(dbAll.value("total", 0)) : dbHelm.dump().substr(0, 300));
			std::string statMode = okDbo && dbo["sort"].size() >= 2 ? dbo["sort"][1].value("sortMode", "") : "";
			bool okStat = okHelm && !statMode.empty() && child.Call("item_db", json{{"kind", "unique"}, {"slot", helmIdx}, {"obtainable", 2}, {"sortMode", statMode}, {"page", 1}, {"size", 4}}, dbStat, 300000);
			bool descending = okStat && dbStat.value("statSort", false) && dbStat["items"].size() >= 2;
			if (descending) for (size_t i = 1; i < dbStat["items"].size(); i++) if (dbStat["items"][i].value("measuredPower", 0.0) > dbStat["items"][i - 1].value("measuredPower", 0.0)) descending = false;
			json dbTooMany;
			bool okTooMany = okStat && child.Call("item_db", json{{"kind", "unique"}, {"obtainable", 2}, {"sortMode", statMode}, {"page", 1}, {"size", 4}}, dbTooMany, 120000);
			check("item_db{sortMode=<stat>}: ListBuilder's GetMiscCalculator sort (measuredPower descending) within the cap, tooMany above it",
			      descending && okTooMany && dbTooMany.value("tooMany", false) && dbTooMany["items"].empty(),
			      okStat ? "mode=" + statMode + " first=" + std::to_string(dbStat["items"].size() ? dbStat["items"][0].value("measuredPower", 0.0) : 0.0) : dbStat.dump().substr(0, 300));
			bool okReq = okHelm && child.Call("item_db", json{{"kind", "unique"}, {"obtainable", 2}, {"requirement", 3}, {"page", 1}, {"size", 5}}, dbReq, 120000) &&
			             child.Call("item_db", json{{"kind", "rare"}, {"page", 1}, {"size", 3}}, dbRare, 120000);
			check("item_db{requirement}: attribute requirements filter against the build's stats; the rare templates list too",
			      okReq && dbReq.value("total", 0) > 0 && dbReq.value("total", 0) < dbAll.value("total", 0) && dbRare["items"].size() == 3,
			      okReq ? "req=" + std::to_string(dbReq.value("total", 0)) + " rare=" + std::to_string(dbRare.value("total", 0)) : dbReq.dump().substr(0, 300));
		}

		// --- 2c: skills ----------------------------------------------------------
		{
			json sk;
			bool okSk = okLoad && child.Call("list_skills", json::object(), sk, 60000);
			int gemsTotal = 0, resolved = 0;
			if (okSk) for (auto& g : sk["groups"]) for (auto& gm : g["gems"]) { gemsTotal++; if (!gm.contains("errMsg") && gm.contains("nameZh")) resolved++; }
			check("list_skills: socket groups with POB-resolved gems, skill sets and the slot options",
			      okSk && sk["groups"].size() >= 1 && gemsTotal >= 3 && resolved == gemsTotal && sk["skillSets"].size() >= 1 && sk["slotOptions"].size() == 13 &&
			          sk.value("mainSocketGroup", 0) >= 1,
			      okSk ? "groups=" + std::to_string(sk["groups"].size()) + " gems=" + std::to_string(gemsTotal) + " resolved=" + std::to_string(resolved)
			           : sk.dump().substr(0, 300));
			json gz, ge;
			bool okGz = okLoad && child.Call("gem_search", json{{"query", u8"\u706b\u7403"}, {"limit", 5}}, gz, 60000);
			bool okGe = okLoad && child.Call("gem_search", json{{"query", "spell echo"}, {"limit", 5}, {"supportOnly", true}}, ge, 60000);
			check("gem_search finds gems by translated and English names",
			      okGz && gz["gems"].size() >= 1 && gz["gems"][0].value("name", "") == "Fireball" && okGe && ge["gems"].size() >= 1 && ge["gems"][0].value("support", false),
			      (okGz ? "zh=" + std::to_string(gz["gems"].size()) : gz.dump().substr(0, 100)) + (okGe ? " en=" + std::to_string(ge["gems"].size()) : ge.dump().substr(0, 100)));

			// a new group with Fireball + Spell Echo becomes the main skill, DPS changes, delete restores
			json st0;
			child.Call("get_stats", json::object(), st0, 30000);
			const int mainBefore = okSk ? sk.value("mainSocketGroup", 1) : 1;
			json ag;
			bool okAg = okSk && child.Call("add_group", json{{"label", "bridge test"}, {"gems", json::array({json{{"nameSpec", "Fireball"}}, json{{"nameSpec", "Spell Echo"}}})}}, ag, 60000);
			const int newIdx = okAg ? ag.value("index", 0) : 0;
			json sk2;
			bool okSk2 = okAg && child.Call("list_skills", json::object(), sk2, 60000);
			bool gemsOk = okSk2 && newIdx >= 1 && sk2["groups"].size() >= (size_t)newIdx && sk2["groups"][newIdx - 1]["gems"].size() == 2 &&
			              sk2["groups"][newIdx - 1]["gems"][0].value("name", "") == "Fireball" && !sk2["groups"][newIdx - 1]["gems"][0].contains("errMsg") &&
			              sk2["groups"][newIdx - 1]["gems"][1].value("support", false) && sk2["groups"][newIdx - 1]["skills"].size() == 1;
			check("add_group resolves gem names through POB (FindSkillGem) and lists the active skill", gemsOk,
			      okSk2 ? sk2["groups"][newIdx > 0 ? newIdx - 1 : 0].dump().substr(0, 300) : ag.dump().substr(0, 300));
			json tt;
			bool okTt = gemsOk && child.Call("gem_tooltip", json{{"group", newIdx}, {"index", 1}}, tt, 60000);
			check("gem_tooltip: POB's gem tooltip for the new gem", okTt && tt["lines"].size() >= 3, okTt ? "" : tt.dump().substr(0, 200));
			json sm, st1;
			bool okSm = gemsOk && child.Call("set_build_field", json{{"field", "mainSocketGroup"}, {"value", newIdx}}, sm, 60000) &&
			            child.Call("get_stats", json::object(), st1, 30000);
			check("making the new group the main skill changes TotalDPS",
			      okSm && st0["stats"].value("TotalDPS", 0.0) != st1["stats"].value("TotalDPS", 0.0),
			      okSm ? "dps " + st0["stats"].value("TotalDPS", json()).dump() + " -> " + st1["stats"].value("TotalDPS", json()).dump() : sm.dump().substr(0, 200));
			json sg, st2;
			bool okSg = okSm && child.Call("set_gem", json{{"group", newIdx}, {"index", 1}, {"level", 1}}, sg, 60000) && child.Call("get_stats", json::object(), st2, 30000);
			check("set_gem{level} recalculates", okSg && st1["stats"].value("TotalDPS", 0.0) != st2["stats"].value("TotalDPS", 0.0));

			// the gem picker sorted by DPS (GemSelectControl's own sort cache + DPS coroutine)
			json gd;
			const auto tGd = GetTickCount64();
			bool okGd = gemsOk && child.Call("gem_search", json{{"group", newIdx}, {"index", 1}, {"query", "Fire"}, {"byDps", true}, {"limit", 8}}, gd, 600000);
			bool dpsOk = okGd && gd.value("byDps", false) && !gd["gems"].empty();
			if (dpsOk) {
				bool anyDps = false;
				for (auto& g : gd["gems"]) if (g.contains("dps") && g["dps"].is_number()) anyDps = true;
				dpsOk = anyDps;
			}
			check("gem_search{byDps} runs POB's gem sort (its DPS estimate per gem) for that socket",
			      dpsOk, okGd ? gd["gems"].dump().substr(0, 240) + " ms=" + std::to_string(GetTickCount64() - tGd) : gd.dump().substr(0, 200));

			// CopySocketGroup / PasteSocketGroup through the bridge (no clipboard)
			json cg, pg, sk4, dpg;
			bool okCg = gemsOk && child.Call("copy_group", json{{"index", newIdx}}, cg, 60000);
			bool okPg = okCg && child.Call("paste_group", json{{"text", cg.value("text", "")}}, pg, 60000);
			bool okSk4 = okPg && child.Call("list_skills", json::object(), sk4, 60000);
			const int pastedIdx = okPg ? pg.value("index", 0) : 0;
			bool pasteOk = okSk4 && cg.value("text", "").find("Fireball") != std::string::npos && pastedIdx == newIdx + 1 &&
			               sk4["groups"].size() > (size_t)newIdx && sk4["groups"][pastedIdx - 1]["gems"].size() == 2 &&
			               sk4["groups"][pastedIdx - 1].value("label", "") == "bridge test";
			json badPaste;
			bool okBadPaste = child.Call("paste_group", json{{"text", "not a gem list"}}, badPaste, 60000);
			check("copy_group gives CopySocketGroup's text and paste_group adds the same group back; text without gems is refused",
			      pasteOk && !okBadPaste && child.Alive(), cg.dump().substr(0, 160) + " " + pg.dump().substr(0, 100));
			if (pastedIdx > 0) child.Call("delete_group", json{{"index", pastedIdx}}, dpg, 60000);

			// Gem Options
			json go0, go1, go2;
			bool okGo = child.Call("get_gem_options", json::object(), go0, 30000) &&
			            child.Call("set_gem_options", json{{"defaultGemQuality", 17}, {"showLegacyGems", !go0.value("showLegacyGems", false)}, {"defaultGemLevel", "corruptedMaximum"}}, go1, 30000);
			bool goOk = okGo && go1.value("defaultGemQuality", -1) == 17 && go1.value("showLegacyGems", false) != go0.value("showLegacyGems", false) &&
			            go1.value("defaultGemLevel", "") == "corruptedMaximum" && go0["sortFields"].size() >= 5 && go0["defaultGemLevels"].size() >= 2 &&
			            go0["supportGemTypes"].size() >= 2;
			json badGo;
			bool okBadGo = child.Call("set_gem_options", json{{"defaultGemLevel", "no such level"}}, badGo, 30000);
			child.Call("set_gem_options", json{{"defaultGemQuality", go0.value("defaultGemQuality", 0)}, {"showLegacyGems", go0.value("showLegacyGems", false)},
			                                   {"defaultGemLevel", go0.value("defaultGemLevel", "normalMaximum")}}, go2, 30000);
			check("get/set_gem_options drive SkillsTab's Gem Options controls (and refuse an unknown level)",
			      goOk && !okBadGo && go2.value("defaultGemQuality", -1) == go0.value("defaultGemQuality", -2), go1.dump().substr(0, 240));

			// group extras: count only for item groups; imbued support on a slotted group (PoE1)
			json gx0, ss1, isup, gx1, iclr, gx2, ss2;
			bool okGx = gemsOk && child.Call("group_extras", json{{"index", newIdx}}, gx0, 30000);
			bool okImb = okGx && child.Call("set_group", json{{"index", newIdx}, {"slot", "Helmet"}}, ss1, 60000) &&
			             child.Call("set_imbued_support", json{{"index", newIdx}, {"gemId", "Metadata/Items/Gems/SkillGemSupportControlledDestruction"}}, isup, 60000) &&
			             child.Call("group_extras", json{{"index", newIdx}}, gx1, 30000) &&
			             child.Call("set_imbued_support", json{{"index", newIdx}}, iclr, 60000) &&
			             child.Call("group_extras", json{{"index", newIdx}}, gx2, 30000) &&
			             child.Call("set_group", json{{"index", newIdx}, {"slot", ""}}, ss2, 60000);
			json cnt;
			bool okCnt = child.Call("set_group_count", json{{"index", newIdx}, {"count", 3}}, cnt, 30000);
			check("group_extras + set_imbued_support: a slotted group takes an imbued support and clears it; count is refused for a socketed group",
			      okImb && !gx0.value("countShown", true) && gx1["imbued"].value("name", "") != "" && !gx2["imbued"].contains("name") && !okCnt && child.Alive(),
			      gx1.dump().substr(0, 200) + " " + isup.dump().substr(0, 160));
			json dg, st3, sk3;
			bool okDg = okSm && child.Call("set_build_field", json{{"field", "mainSocketGroup"}, {"value", mainBefore}}, dg, 60000) &&
			            child.Call("delete_group", json{{"index", newIdx}}, dg, 60000) && child.Call("get_stats", json::object(), st3, 30000) &&
			            child.Call("list_skills", json::object(), sk3, 60000);
			check("restoring the main group and deleting the new one restores the group count and TotalDPS",
			      okDg && sk3["groups"].size() == sk["groups"].size() && st0["stats"].value("TotalDPS", 0.0) == st3["stats"].value("TotalDPS", 1.0),
			      okDg ? "groups=" + std::to_string(sk3["groups"].size()) + " dps " + st3["stats"].value("TotalDPS", json()).dump() : dg.dump().substr(0, 200));
		}

		// --- 2d: config + calcs ---------------------------------------------------
		{
			json cf;
			bool okCf = okLoad && child.Call("list_config", json::object(), cf, 60000);
			int items = 0, visible = 0, lists = 0, zhLabels = 0;
			if (okCf) for (auto& sec : cf["sections"]) for (auto& it : sec["items"]) {
				items++;
				if (it.value("visible", false)) visible++;
				if (it.contains("list")) lists++;
				if (it.value("labelZh", "") != it.value("label", "")) zhLabels++;
			}
			check("list_config: POB's option list in sections, with POB's own visibility and translated labels",
			      okCf && cf["sections"].size() >= 5 && items > 200 && visible > 20 && visible < items && lists > 5 && zhLabels > 50 &&
			          cf["configSets"].size() >= 1 && cf["customMods"].size() >= 1,
			      okCf ? "sections=" + std::to_string(cf["sections"].size()) + " items=" + std::to_string(items) + " visible=" + std::to_string(visible) +
			                 " zh=" + std::to_string(zhLabels)
			           : cf.dump().substr(0, 300));
			json st0, sc, st1, rc, st2;
			child.Call("get_stats", json::object(), st0, 30000);
			bool okSc = okCf && child.Call("set_config", json{{"var", "enemyIsBoss"}, {"value", "Uber"}}, sc, 60000) && child.Call("get_stats", json::object(), st1, 30000);
			check("set_config{enemyIsBoss=Uber} changes the calculation (BuildModList + recalculation)",
			      okSc && (st0["stats"].value("TotalDPS", 0.0) != st1["stats"].value("TotalDPS", 0.0) || st0["stats"].value("TotalEHP", 0.0) != st1["stats"].value("TotalEHP", 0.0)),
			      okSc ? "dps " + st0["stats"].value("TotalDPS", json()).dump() + " -> " + st1["stats"].value("TotalDPS", json()).dump() : sc.dump().substr(0, 200));
			bool okRc = okSc && child.Call("reset_config", json{{"var", "enemyIsBoss"}}, rc, 60000) && child.Call("get_stats", json::object(), st2, 30000);
			check("reset_config restores the default and the numbers", okRc && st0["stats"].value("TotalDPS", 0.0) == st2["stats"].value("TotalDPS", 1.0) &&
			                                                              st0["stats"].value("TotalEHP", 0.0) == st2["stats"].value("TotalEHP", 1.0));
			json cm, st3, cm2, st4;
			bool okCm = okCf && child.Call("set_custom_mods", json{{"list", json::array({json{{"title", "t"}, {"text", "100% increased Damage"}, {"enabled", true}}})}}, cm, 60000) &&
			            child.Call("get_stats", json::object(), st3, 30000) &&
			            child.Call("set_custom_mods", json{{"list", json::array()}}, cm2, 60000) && child.Call("get_stats", json::object(), st4, 30000);
			check("set_custom_mods parses a modifier line through POB and clearing it restores TotalDPS",
			      okCm && st3["stats"].value("TotalDPS", 0.0) > st0["stats"].value("TotalDPS", 0.0) && st0["stats"].value("TotalDPS", 0.0) == st4["stats"].value("TotalDPS", 1.0),
			      okCm ? "dps " + st0["stats"].value("TotalDPS", json()).dump() + " -> " + st3["stats"].value("TotalDPS", json()).dump() : cm.dump().substr(0, 200));

			// the Add Mod browser behind the custom-modifier group
			json ms, ma, lc2, st5, cmReset;
			bool okMs = okCf && child.Call("config_mod_search", json{{"block", 1}, {"query", "increased attack speed"}, {"limit", 20}}, ms, 60000);
			const std::string pick = (okMs && !ms["mods"].empty()) ? ms["mods"][0].value("text", "") : "";
			bool okMa = !pick.empty() && child.Call("config_mod_add", json{{"block", 1}, {"text", pick}}, ma, 60000) &&
			            child.Call("list_config", json::object(), lc2, 60000);
			bool added = false;
			if (okMa) for (auto& blk : lc2["customMods"]) if (blk.value("text", "").find(pick) != std::string::npos) added = true;
			json msBad;
			bool okMsBad = child.Call("config_mod_add", json{{"block", 1}, {"text", "not a modifier at all"}}, msBad, 60000);
			child.Call("set_custom_mods", json{{"list", json::array()}}, cmReset, 60000);
			check("config_mod_search/config_mod_add drive POB's Mod Browser (its fuzzy search, its Add button); an unknown line is refused",
			      okMs && !ms["mods"].empty() && added && !okMsBad && child.Alive(),
			      "pick=" + pick + " total=" + std::to_string(ms.value("total", -1)) + " " + (okMa ? lc2["customMods"].dump().substr(0, 160) : ma.dump().substr(0, 160)));

			json cal;
			bool okCal = okLoad && child.Call("get_calcs", json::object(), cal, 60000);
			int rows = 0, cells = 0, withBd = 0, enabledSecs = 0;
			int bdSi = 0, bdUi = 0, bdRi = 0, bdCi = 0;
			if (okCal) for (auto& sec : cal["sections"]) {
				if (sec.value("enabled", false)) enabledSecs++;
				for (auto& sub : sec["subsections"]) for (auto& row : sub["rows"]) {
					rows++;
					for (auto& c : row["cells"]) {
						cells++;
						if (c.value("hasBreakdown", false)) {
							withBd++;
							if (!bdSi && c.contains("text") && !c.value("text", "").empty()) { bdSi = sec.value("si", 0); bdUi = sub.value("ui", 0); bdRi = row.value("ri", 0); bdCi = c.value("ci", 0); }
						}
					}
				}
			}
			check("get_calcs: POB's calc sections formatted by formatCalcStr, rows filtered by CheckFlag",
			      okCal && cal["sections"].size() > 20 && enabledSecs > 10 && rows > 100 && cells > 150 && withBd > 100 && cal["selectors"].contains("mainSocketGroup"),
			      okCal ? "sections=" + std::to_string(cal["sections"].size()) + " rows=" + std::to_string(rows) + " cells=" + std::to_string(cells) + " bd=" + std::to_string(withBd)
			            : cal.dump().substr(0, 300));
			json bd;
			bool okBd = bdSi > 0 && child.Call("calcs_breakdown", json{{"si", bdSi}, {"ui", bdUi}, {"ri", bdRi}, {"ci", bdCi}}, bd, 60000);
			check("calcs_breakdown: a cell's breakdown sections through CalcBreakdownControl", okBd && bd["sections"].size() >= 1,
			      okBd ? "sections=" + std::to_string(bd["sections"].size()) : bd.dump().substr(0, 200));
			json ci, cal2;
			bool okCi = okCal && child.Call("set_calcs_input", json{{"var", "misc_buffMode"}, {"value", "UNBUFFED"}}, ci, 60000) &&
			            child.Call("get_calcs", json::object(), cal2, 60000) && child.Call("set_calcs_input", json{{"var", "misc_buffMode"}, {"value", "EFFECTIVE"}}, ci, 60000);
			check("set_calcs_input{misc_buffMode} round-trips", okCi && cal2["input"].value("misc_buffMode", "") == "UNBUFFED");
		}

		// --- 2e: notes + party ------------------------------------------------------
		{
			json n0, sn, n1, bi;
			bool okN = okLoad && child.Call("get_notes", json::object(), n0, 30000) &&
			           child.Call("set_notes", json{{"text", "bridge notes ^xFF0000red\nline two"}}, sn, 60000) &&
			           child.Call("get_notes", json::object(), n1, 30000) && child.Call("get_build_info", json::object(), bi, 30000);
			check("set_notes/get_notes round-trip through NotesTab's edit control and marks the build unsaved",
			      okN && n1.value("text", "") == "bridge notes ^xFF0000red\nline two" && n1.value("unsaved", false) && bi.value("unsaved", false),
			      okN ? n1.dump().substr(0, 120) : sn.dump().substr(0, 200));
			bool colourCodes = okN && n0["colours"].is_array() && n0["colours"].size() == 12;
			if (colourCodes) for (auto& c : n0["colours"]) if (c.value("code", "").rfind("^", 0) != 0 || c.value("name", "").empty()) colourCodes = false;
			check("get_notes lists NotesTab's 12 colour buttons (code + name read from their labels)", colourCodes,
			      okN ? n0["colours"].dump().substr(0, 200) : "");
			json ex, dec;
			bool okEx = okN && child.Call("export_code", json::object(), ex, 60000) && child.Call("decode_code", json{{"code", ex.value("code", "")}}, dec, 60000);
			bool hasNotes = false;
			if (okEx) for (auto& x : dec["sections"]) if (x.get<std::string>() == "Notes") hasNotes = true;
			check("the notes travel in the build (export_code carries a <Notes> section)", okEx && hasNotes);
			json rn;
			child.Call("set_notes", json{{"text", n0.value("text", "")}}, rn, 60000);

			json p0, sp, p1, sp2, p2;
			bool okP = okLoad && child.Call("get_party", json::object(), p0, 60000) &&
			           child.Call("set_party", json{{"field", "enemyCond"}, {"text", "Condition:Shocked"}}, sp, 60000) &&
			           child.Call("get_party", json::object(), p1, 60000) &&
			           child.Call("set_party", json{{"field", "enemyCond"}, {"text", ""}}, sp2, 60000) &&
			           child.Call("get_party", json::object(), p2, 60000);
			check("set_party/get_party round-trip through PartyTab's controls and ParseBuffs",
			      okP && p0["fields"].contains("aura") && p0["fields"].size() == 7 && p1["fields"].value("enemyCond", "") == "Condition:Shocked" &&
			          p1.value("unsaved", false) && p2["fields"].value("enemyCond", "x").empty(),
			      okP ? p1["fields"].dump().substr(0, 160) : sp.dump().substr(0, 200));
			json pe;
			bool okPe = okP && child.Call("set_party", json{{"field", "enableExportBuffs"}, {"value", true}}, pe, 60000) && child.Call("get_party", json::object(), pe, 60000);
			check("enableExportBuffs exposes this build's exported buff text", okPe && pe.value("enableExportBuffs", false) && pe.contains("exports"),
			      okPe ? "exports=" + std::to_string(pe["exports"].size()) : pe.dump().substr(0, 200));
			// this build's own export (with its buffs) imported into the party lists, then Clear
			json ec, pst, pim, pgot, pclr, pafter, pbad;
			bool okEc = okPe && child.Call("export_code", json::object(), ec, 60000);
			child.Call("set_party", json{{"field", "enableExportBuffs"}, {"value", false}}, pe, 60000);
			bool okPst = okEc && child.Call("party_import_state", json::object(), pst, 30000);
			bool okPim = okPst && child.Call("party_import", json{{"code", ec.value("code", "")}, {"destination", 1}}, pim, 60000) &&
			             child.Call("get_party", json::object(), pgot, 60000);
			bool okPclr = okPim && child.Call("party_action", json{{"action", "clear"}}, pclr, 60000) && child.Call("get_party", json::object(), pafter, 60000);
			bool okPbad = child.Call("party_import", json{{"code", "not a code"}}, pbad, 30000);
			bool anyFilled = false, allEmpty = okPclr;
			if (okPim) for (auto& [k, v] : pgot["fields"].items()) if (v.is_string() && !v.get<std::string>().empty()) anyFilled = true;
			if (okPclr) for (auto& [k, v] : pafter["fields"].items()) if (v.is_string() && !v.get<std::string>().empty()) allEmpty = false;
			check("party_import brings a build's exported buffs into the party lists (destination All); Clear empties them; a bad code is refused",
			      okPim && pst["destinations"].size() == 8 && pim.value("valid", false) && anyFilled && allEmpty && !okPbad && child.Alive(),
			      (okPim ? pgot["fields"].dump() : pim.dump()).substr(0, 240) + " " + pbad.dump().substr(0, 100));
			json pdis, preb;
			bool okAct = child.Call("party_action", json{{"action", "disable"}}, pdis, 60000) && child.Call("party_action", json{{"action", "rebuild"}}, preb, 60000);
			json pbadAct;
			bool okBadAct = child.Call("party_action", json{{"action", "explode"}}, pbadAct, 30000);
			check("party_action disable/rebuild run PartyTab's buttons; an unknown action is refused", okAct && !okBadAct && child.Alive(), pdis.dump().substr(0, 120));
			json ab;
			bool okAb = child.Call("about", json::object(), ab, 60000);
			check("about lists POB's version history and help text (main:OpenAboutPopup's lists)",
			      okAb && ab["changelog"].size() > 20 && ab["help"].size() > 20 && ab.value("version", "").find("Path of Building") != std::string::npos,
			      okAb ? "changelog=" + std::to_string(ab["changelog"].size()) + " help=" + std::to_string(ab["help"].size()) : ab.dump().substr(0, 200));
		}

		// Find a Timeless Jewel: POB's dialog driven through the bridge
		{
			json tj0, tj1, tjs, tjr, tjc;
			const auto tTj = GetTickCount64();
			bool okTj = okLoad && child.Call("tj_open", json::object(), tj0, 120000);
			const size_t sockets = okTj ? tj0["socket"]["options"].size() : 0;
			bool okSet = okTj && sockets > 1 &&
			             child.Call("tj_set", json{{"jewel", 2}, {"socket", 2}, {"searchList", "1 Added Passive Skill is Concentrated Effect"}}, tj1, 120000);
			bool okSearch = okSet && child.Call("tj_search", json::object(), tjs, 600000);
			bool okRaw = true;
			if (okSearch && tjs.value("resultCount", 0) > 0) {
				okRaw = child.Call("tj_result", json{{"index", 1}}, tjr, 60000) && tjr.value("raw", "").find("Lethal Pride") != std::string::npos;
			}
			// every socket option carries its tree node (the page maps where it is);
			// the first is "All Sockets" (-1), the rest are real jewel sockets
			bool idsOk = okTj && tj0.contains("socketIds") && tj0["socketIds"].size() == sockets && sockets > 1 &&
			             tj0["socketIds"][0].get<int>() == -1;
			int realIds = 0;
			if (idsOk) for (size_t i = 1; i < sockets; i++) if (tj0["socketIds"][i].get<int>() > 0) realIds++;
			check("tj_open: every socket option names its tree node (for the page's socket map)",
			      idsOk && realIds == (int)sockets - 1,
			      "sockets=" + std::to_string(sockets) + " ids=" + (okTj && tj0.contains("socketIds") ? tj0["socketIds"].dump().substr(0, 120) : std::string("none")));
			child.Call("tj_close", json::object(), tjc, 30000);
			check("tj_open/tj_set/tj_search: POB's timeless jewel dialog answers with its jewel types, sockets and seed results",
			      okTj && tj0["jewel"]["options"].size() >= 5 && sockets >= 1 && okSet && okSearch && okRaw && child.Alive(),
			      "sockets=" + std::to_string(sockets) + " results=" + std::to_string(okSearch ? tjs.value("resultCount", -1) : -1) +
			          " ms=" + std::to_string(GetTickCount64() - tTj) + " " + (okTj ? "" : tj0.dump().substr(0, 200)));
		}

		// the shared item list and shared item sets (main's two shared lists)
		{
			json li2, sh1, sh2, use, un1, un2, shEnd;
			bool okLi = okLoad && child.Call("list_items", json::object(), li2, 60000);
			const int shareId = (okLi && !li2["items"].empty()) ? li2["items"][0].value("id", 0) : 0;
			const int setId = okLi && !li2["itemSets"].empty() ? li2["itemSets"][0].value("id", 0) : 0;
			bool okSh = shareId > 0 && child.Call("share_item", json{{"id", shareId}}, sh1, 60000);
			bool okShSet = okSh && setId > 0 && child.Call("share_item_set", json{{"id", setId}}, sh2, 60000);
			bool okUse = okShSet && child.Call("use_shared_set", json{{"index", 1}}, use, 60000);
			const int newSetId = okUse ? use.value("id", 0) : 0;
			json delSet;
			if (newSetId > 0) child.Call("delete_item_set", json{{"id", newSetId}}, delSet, 60000);
			bool okUn = okShSet && child.Call("unshare", json{{"kind", "set"}, {"index", 1}}, un1, 30000) &&
			            child.Call("unshare", json{{"kind", "item"}, {"index", 1}}, un2, 30000) &&
			            child.Call("shared_items", json::object(), shEnd, 30000);
			check("shared items: an item and an item set go to POB's shared lists, a shared set comes back as a new set, and both unshare",
			      okSh && sh1["items"].size() == 1 && okShSet && sh2["sets"].size() == 1 && !sh2["sets"][0]["slots"].empty() &&
			          okUse && newSetId > 0 && okUn && shEnd["items"].empty() && shEnd["sets"].empty(),
			      "id=" + std::to_string(shareId) + " set=" + std::to_string(setId) + " new=" + std::to_string(newSetId) + " " + sh2.dump().substr(0, 160));
		}

		// the spectre library (Build.lua's Manage Spectres) and PoE2's stat sets
		{
			json ml, sml, ml2, mlBad;
			bool okMl = okLoad && child.Call("minion_library", json{{"kind", "spectre"}}, ml, 60000);
			const std::string firstSpectre = (okMl && !ml["available"].empty()) ? ml["available"][0].value("id", "") : "";
			bool okSml = !firstSpectre.empty() && child.Call("set_minion_library", json{{"kind", "spectre"}, {"ids", json::array({firstSpectre})}}, sml, 60000) &&
			             child.Call("minion_library", json{{"kind", "spectre"}}, ml2, 60000);
			bool okBadMl = child.Call("set_minion_library", json{{"kind", "spectre"}, {"ids", json::array({"NotAMinion"})}}, mlBad, 30000);
			check("minion_library/set_minion_library: POB's spectre library (its own data.spectres), and an unknown minion is refused",
			      okMl && ml["available"].size() > 50 && okSml && ml2["inBuild"].size() == 1 &&
			          ml2["inBuild"][0].value("id", "") == firstSpectre && !ml2["inBuild"][0].value("name", "").empty() && !okBadMl && child.Alive(),
			      "avail=" + std::to_string(okMl ? ml["available"].size() : 0) + " first=" + firstSpectre);
		}

		// per-tab undo/redo, and the build sites POB knows
		{
			json us, ag2, sk5, un, re, sk6, sk7, dgU, badTab;
			bool okUs = okLoad && child.Call("undo_state", json::object(), us, 30000);
			bool okAg2 = okUs && child.Call("add_group", json{{"label", "undo test"}, {"gems", json::array({json{{"nameSpec", "Fireball"}}})}}, ag2, 60000) &&
			             child.Call("list_skills", json::object(), sk5, 60000);
			const size_t withGroup = okAg2 ? sk5["groups"].size() : 0;
			bool okUn = okAg2 && child.Call("tab_undo", json{{"tab", "skills"}}, un, 60000) && child.Call("list_skills", json::object(), sk6, 60000);
			bool okRe = okUn && child.Call("tab_undo", json{{"tab", "skills"}, {"redo", true}}, re, 60000) && child.Call("list_skills", json::object(), sk7, 60000);
			if (okRe && sk7["groups"].size() == withGroup) child.Call("delete_group", json{{"index", (int)withGroup}}, dgU, 60000);
			bool okBadTab = child.Call("tab_undo", json{{"tab", "nowhere"}}, badTab, 30000);
			check("tab_undo/undo_state: each tab's own UndoHandler (Ctrl+Z / Ctrl+Y); an unknown tab is refused",
			      okUs && us["tabs"].contains("items") && us["tabs"].contains("skills") && okUn && sk6["groups"].size() + 1 == withGroup &&
			          okRe && sk7["groups"].size() == withGroup && !okBadTab && child.Alive(),
			      "with=" + std::to_string(withGroup) + " undo=" + std::to_string(okUn ? sk6["groups"].size() : 0) +
			          " redo=" + std::to_string(okRe ? sk7["groups"].size() : 0));
			json bsites, badUrl;
			bool okBs = child.Call("list_build_sites", json::object(), bsites, 30000);
			int shareable = 0;
			if (okBs) for (auto& s : bsites["sites"]) if (s.value("canShare", false)) shareable++;
			bool okBadUrl = child.Call("import_from_url", json{{"url", "https://example.com/not-a-build"}}, badUrl, 30000);
			check("list_build_sites lists POB's build sites (and which take uploads); a link from elsewhere is refused",
			      okBs && bsites["sites"].size() >= 5 && shareable >= 2 && !okBadUrl && child.Alive(),
			      okBs ? bsites["sites"].dump().substr(0, 200) : bsites.dump().substr(0, 200));
		}

		// the loadout drop-down (Build.lua SyncLoadouts)
		json lo, lo2;
		bool okLo = okLoad && child.Call("list_loadouts", json::object(), lo, 60000);
		int newLoadout = 0;
		if (okLo) for (auto& e : lo["entries"]) if (e.value("label", "") == "New Loadout") newLoadout = e.value("index", 0);
		bool okLo2 = newLoadout > 0 && child.Call("select_loadout", json{{"index", newLoadout}, {"title", "bridge loadout"}}, lo2, 60000);
		bool madeLoadout = false;
		if (okLo2) for (auto& e : lo2["entries"]) if (e.value("label", "").find("bridge loadout") != std::string::npos) madeLoadout = true;
		int newAfter = 0, madeIdx = 0;
		if (okLo2) for (auto& e : lo2["entries"]) {
			if (e.value("label", "") == "New Loadout") newAfter = e.value("index", 0);
			if (e.value("label", "").find("bridge loadout") != std::string::npos) madeIdx = e.value("index", 0);
		}
		json loBad, loPick;
		bool okLoBad = newAfter > 0 && child.Call("select_loadout", json{{"index", newAfter}}, loBad, 60000);
		bool okLoPick = madeIdx > 0 && child.Call("select_loadout", json{{"index", madeIdx}}, loPick, 60000);
		check("list_loadouts/select_loadout: POB's loadout list, and New Loadout answered with a name adds one and it can be switched to (a nameless one is refused)",
		      okLo && lo["entries"].size() >= 2 && madeLoadout && okLoPick && !okLoBad && child.Alive(),
		      "new=" + std::to_string(newLoadout) + " made=" + std::to_string(madeLoadout) + " bad=" + std::to_string(okLoBad) + " " +
                  (okLo2 ? lo2["entries"].dump().substr(0, 260) : lo2.dump().substr(0, 200)));

		// back to the loadout the build had, so the rest of the run sees its sets
		{
			json back;
			child.Call("select_loadout", json{{"index", 2}}, back, 60000);
		}

		// --- POB's Options dialog: read its controls, save through its Save -----
		{
			json po, ver, s1, s2;
			bool okPo = okLoad && child.Call("pob_options", json::object(), po, 60000) && child.Call("version", json::object(), ver, 30000);
			bool hasBeta = false, hasProto = false, hasDpi = false, proxyLabelled = false, sepState = false, bothSections = false;
			int n = 0, app = 0, bld = 0;
			if (okPo) for (auto& o : po["options"]) {
				n++;
				const std::string name = o.value("name", ""), kind = o.value("kind", ""), sec = o.value("section", "");
				if (sec == "app") app++; else if (sec == "build") bld++;
				if (name == "betaTest" && kind == "check") hasBeta = true;
				if (name == "connectionProtocol" && kind == "dropdown" && o["options"].size() == 3) hasProto = true;
				if (name == "dpiScaleOverride") hasDpi = true;
				if (name == "proxyType" && o.contains("label")) proxyLabelled = true;
				if (name == "showThousandsSeparators") sepState = o.value("state", false);
			}
			bothSections = app >= 10 && bld >= 10;
			check("pob_options: main:OpenOptionsPopup's controls in its two sections (beta opt-in, protocol, captions), the DPI override left out",
			      okPo && n >= 25 && bothSections && hasBeta && hasProto && !hasDpi && proxyLabelled,
			      okPo ? "options=" + std::to_string(n) + " app=" + std::to_string(app) + " build=" + std::to_string(bld) : po.dump().substr(0, 300));
			// Save also writes the manifest branch from the beta box: keep it on the branch the sandbox has
			const bool beta = okPo && ver.value("pobBranch", "") == "beta";
			bool okS1 = okPo && child.Call("set_pob_options", json{{"values", json{{"showThousandsSeparators", !sepState}, {"betaTest", beta}}}}, s1, 60000);
			bool flipped = false;
			if (okS1) for (auto& o : s1["options"]) if (o.value("name", "") == "showThousandsSeparators") flipped = o.value("state", sepState) == !sepState;
			bool okS2 = okS1 && child.Call("set_pob_options", json{{"values", json{{"showThousandsSeparators", sepState}, {"betaTest", beta}}}}, s2, 60000);
			bool restored = false;
			if (okS2) for (auto& o : s2["options"]) if (o.value("name", "") == "showThousandsSeparators") restored = o.value("state", !sepState) == sepState;
			json bad;
			bool okBad = child.Call("set_pob_options", json{{"values", json{{"noSuchOption", 1}}}}, bad, 60000);
			check("set_pob_options goes through the dialog's callbacks and Save (a toggle round-trips) and refuses unknown names",
			      okS1 && s1.value("saved", false) && flipped && okS2 && restored && !okBad && child.Alive(),
			      okS1 ? std::string("flipped=") + (flipped ? "1" : "0") + " restored=" + (restored ? "1" : "0") : s1.dump().substr(0, 300));
			json ui;
			bool okUi = child.Call("pob_update_info", json::object(), ui, 60000);
			// POB lists the entries newer than the running version: none on an
			// up-to-date master, the whole file on beta (its version has a hash)
			check("pob_update_info: main:OpenUpdatePopup's changelog list (capped) and the running version",
			      okUi && ui.contains("lines") && ui["lines"].size() <= 400 && !ui.value("version", "").empty(),
			      okUi ? "lines=" + std::to_string(ui["lines"].size()) : ui.dump().substr(0, 200));
		}

		// --- POB's own update check, synchronously -----------------------------
		json upd;
		bool okUpd = child.Call("check_update_sync", json::object(), upd, 300000);
		check("check_update_sync answered", okUpd, upd.dump());
		if (okUpd) {
			// "none" while the sandbox copy is current; upstream POB does release
			// new builds, and then its own check legitimately answers "normal"
			// (or "basic"). Either is a working check; an error is not.
			const std::string mode = upd.value("mode", "");
			check("update check on an untouched sandbox answers without an error (\"none\", or an update POB found)",
			      (mode == "none" || mode == "normal" || mode == "basic") && upd.value("error", "").empty(),
			      "mode=" + mode + " error=" + upd.value("error", ""));
		}
		const bool untouchedNone = okUpd && upd.value("mode", "") == "none";
		const std::string corruption = "\n-- pobtools headless selftest: deliberate corruption\n";

		// --- POB's own "normal" update, end to end --------------------------------
		// A program-part file with a wrong sha1 is exactly what UpdateCheck's
		// integrity pass re-downloads; ApplyUpdate then Restart()s the Lua state
		// in this same process, and the bridge comes back with it.
		if (!untouchedNone) {
			check("normal update round trip", true, "skipped: the untouched sandbox already differs from upstream (a POB update is pending)");
		} else {
			const std::wstring progRel = L"Classes\\Tooltip.lua";
			const std::wstring progFile = sandbox + L"\\" + progRel;
			const std::string manifest = ReadFileA(sandbox + L"\\manifest.xml");
			bool inManifest = manifest.find("name=\"Classes/Tooltip.lua\"") != std::string::npos;
			std::string orig = ReadFileA(progFile);
			bool corrupted = inManifest && !orig.empty() && WriteFileA(progFile, orig + corruption);
			json u1;
			bool okU1 = corrupted && child.Call("check_update_sync", json::object(), u1, 300000);
			check("a program file with the wrong sha1 makes POB's update check say \"normal\"",
			      okU1 && u1.value("mode", "") == "normal",
			      okU1 ? "mode=" + u1.value("mode", "") + " error=" + u1.value("error", "") : (inManifest ? u1.dump().substr(0, 300) : "Classes/Tooltip.lua not in manifest"));
			json ap;
			bool okAp = okU1 && u1.value("mode", "") == "normal" && child.Call("apply_update", json{{"mode", "normal"}}, ap, 300000);
			json evR, evH;
			bool gotRestart = okAp && child.WaitEvent("restarted", evR, 90000);
			bool gotHello = gotRestart && child.WaitEvent("hello", evH, 90000);
			check("apply_update{normal}: POB applied the files and Restart()ed the Lua state (restarted + hello events)",
			      okAp && ap.value("applied", "") == "normal" && gotRestart && gotHello,
			      "applied=" + ap.dump().substr(0, 120) + " restarted=" + std::to_string(gotRestart) + " hello=" + std::to_string(gotHello));
			std::string after = ReadFileA(progFile);
			json ver2, u2;
			bool okVer2 = gotHello && child.Call("version", json::object(), ver2, 60000);
			bool okU2 = okVer2 && child.Call("check_update_sync", json::object(), u2, 300000);
			check("after the restart: the file is POB's again, Update\\opFile.txt is consumed, the bridge answers, and the check says \"none\"",
			      gotHello && after.find(corruption) == std::string::npos && !after.empty() &&
			          GetFileAttributesW((sandbox + L"\\Update\\opFile.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
			          okVer2 && !ver2.value("pobVersion", "").empty() && okU2 && u2.value("mode", "") == "none",
			      "restored=" + std::to_string(after.find(corruption) == std::string::npos) + " version=" + ver2.value("pobVersion", "") + " mode=" + u2.value("mode", "") + " err=" + u2.value("error", ""));
			// the build again, so the shutdown checks below see the same state as before
			json rl2, st5;
			bool okRl2 = gotHello && child.Call("load_build_file", json{{"path", narrow(sandbox + L"\\Builds\\" + sample)}}, rl2, 120000) &&
			             child.Call("get_stats", json::object(), st5, 30000);
			check("the reloaded engine still opens the sample build and calculates it", okRl2 && st5.value("count", 0) > 100,
			      okRl2 ? "stats=" + std::to_string(st5.value("count", 0)) : rl2.dump().substr(0, 200));
			if (corrupted && after.find(corruption) != std::string::npos) WriteFileA(progFile, orig); // never leave the sandbox corrupted
		}

		// the Compare tab: this build compared against its own share code
		{
			json ex2, cl, cs, ctree, citems, cskills, cconf, crm;
			bool okEx = okLoad && child.Call("export_code", json::object(), ex2, 60000);
			bool okCl = okEx && child.Call("compare_load", json{{"code", ex2.value("code", "")}, {"label", "self"}}, cl, 180000);
			bool okCs = okCl && child.Call("compare_state", json::object(), cs, 60000) &&
			            child.Call("compare_tree", json::object(), ctree, 60000) &&
			            child.Call("compare_items", json::object(), citems, 60000) &&
			            child.Call("compare_skills", json::object(), cskills, 60000) &&
			            child.Call("compare_config", json::object(), cconf, 60000);
			bool sameBuild = okCs && ctree["onlyInCompare"].empty() && ctree["onlyInPrimary"].empty() && cconf["rows"].empty();
			bool statsOk = okCs && cs["stats"].size() > 10;
			bool noDiff = true;
			if (statsOk) for (auto& r : cs["stats"]) if (r.contains("diff")) noDiff = false;
			bool itemsSame = okCs && !citems["rows"].empty();
			if (itemsSame) for (auto& r : citems["rows"]) if (r.contains("primary") && r.contains("compare") && !r.value("same", false)) itemsSame = false;
			json cbad;
			bool okBad2 = child.Call("compare_load", json{{"code", "not a code"}}, cbad, 60000);
			if (okCl) child.Call("compare_remove", json{{"index", 1}}, crm, 60000);
			check("compare_load/compare_state: this build against its own code shows no differences (tree, items, config, stats)",
			      okCl && statsOk && sameBuild && noDiff && itemsSame && !okBad2 && child.Alive(),
			      "stats=" + std::to_string(statsOk ? cs["stats"].size() : 0) + " noDiff=" + std::to_string(noDiff) + " itemsSame=" + std::to_string(itemsSame) +
			          " bad=" + std::to_string(okBad2) + " " + [&] {
				          std::string o;
				          if (statsOk) for (auto& r : cs["stats"]) if (r.contains("diff")) o += r.value("label", "?") + "=" + r.value("diff", "") + " ";
				          if (okCs) for (auto& r : citems["rows"]) if (!r.value("same", false)) o += "[" + r.value("slot", "?") + "]";
				          return o.substr(0, 260);
			          }());
		}

		// Buy Similar builds a trade URL from an item; the weights dialog lists POB's stats
		{
			json li3, bs, bsBad, tw, twSet;
			bool okLi3 = okLoad && child.Call("list_items", json::object(), li3, 60000);
			const int buyId = (okLi3 && !li3["items"].empty()) ? li3["items"][0].value("id", 0) : 0;
			bool okBs = buyId > 0 && child.Call("buy_similar", json{{"id", buyId}}, bs, 120000);
			bool okBsBad = child.Call("buy_similar", json{{"id", 999999}}, bsBad, 30000);
			check("buy_similar builds POB's trade search URL for an item; an unknown item is refused",
			      okBs && bs.value("url", "").find("/trade/search") != std::string::npos && !okBsBad && child.Alive(),
			      okBs ? bs.value("url", "").substr(0, 120) : bs.dump().substr(0, 200));
			bool okTw = child.Call("trade_weights", json::object(), tw, 120000);
			const std::string wStat = (okTw && !tw["stats"].empty()) ? tw["stats"][0].value("stat", "") : "";
			bool okTwSet = okTw && !wStat.empty() &&
			               child.Call("trade_weights", json{{"weights", json::array({json{{"stat", wStat}, {"weight", 0.5}}})}}, twSet, 120000);
			bool weighted = false;
			if (okTwSet) for (auto& s2 : twSet["selected"]) if (s2.value("stat", "") == wStat) weighted = true;
			check("trade_weights lists POB's weightable stats and saving one keeps it in the sort selection",
			      okTw && tw["stats"].size() > 5 && okTwSet && weighted,
			      okTw ? "stats=" + std::to_string(tw["stats"].size()) + " stat=" + wStat + " sel=" + (okTwSet ? twSet["selected"].dump().substr(0, 140) : "") : tw.dump().substr(0, 200));
		}

		// the trade pane (no network here: its lists, rows and refusals)
		{
			json tr0, trSet, trBad, trClose;
			bool okTr = okLoad && child.Call("trade_open", json::object(), tr0, 120000);
			bool okTrSet = okTr && child.Call("trade_set", json{{"tradeType", 2}, {"sort", 2}, {"fetchPages", 3}}, trSet, 60000);
			bool okBad = child.Call("trade_price", json{{"row", 1}}, trBad, 60000);
			// "Find best" without a login only builds POB's weighted search URL:
			// that part is local, so it is checked whenever the league list (which
			// does come from the site) arrived.
			// the realm and league lists come from the site through POB's own
			// background request; give them a few pumps before deciding
			for (int i = 0; okTr && i < 5 && tr0["league"]["options"].empty(); i++) {
				json pump;
				if (!child.Call("trade_refresh", json{{"seconds", 5}}, pump, 60000)) break;
				tr0["league"] = pump["league"];
				tr0["rows"] = pump["rows"];
			}
			const bool leagues = okTr && !tr0["league"]["options"].empty();
			json trBest;
			std::string bestUrl;
			bool bestOk = true;
			if (leagues) {
				bestOk = child.Call("trade_find_best", json{{"row", 1}, {"timeout", 240}}, trBest, 300000);
				if (bestOk) for (auto& r : trBest["rows"]) if (r.value("index", 0) == 1) bestUrl = r.value("url", "");
				bestOk = bestOk && !trBest.value("timedOut", false)
				         && bestUrl.find("/trade/search") != std::string::npos && bestUrl.find("?q=") != std::string::npos;
			}
			check(leagues ? "trade_find_best builds POB's weighted search URL for a slot (no login needed)"
			              : "trade_find_best needs a league list from the site; it was not reachable, so the row stays disabled",
			      leagues ? bestOk : (okTr && !tr0["rows"][0].value("canFindBest", true)),
			      "leagues=" + std::to_string(leagues ? tr0["league"]["options"].size() : 0) + " url=" + bestUrl.substr(0, 110));
			// POB's "Query Options" dialog (what "Find best" opens): the page
			// shows it instead of the bridge answering for it, so its controls
			// have to come out, take a mod filter, and go away again.
			json trOpt, trMods, trModsQ, trPick, trMin, trClosed, trClosedMods, trTip;
			bool optOk = false, modsOk = false, pickOk = false, closedOk = false;
			std::string modLabel;
			if (leagues) {
				optOk = child.Call("trade_options_open", json{{"row", 1}}, trOpt, 120000);
				bool hasExec = false, hasMod = false, hasCheck = false;
				if (optOk) for (auto& c : trOpt["controls"]) {
					const std::string kind = c.value("kind", ""), name = c.value("name", "");
					if (name == "generateQuery" && kind == "button") hasExec = true;
					if (kind == "mod") hasMod = true;
					if (kind == "check") hasCheck = true;
				}
				optOk = optOk && hasExec && hasMod && hasCheck;
				modsOk = optOk && child.Call("trade_options_mods", json{{"prefix", "modSelector"}}, trMods, 60000)
				         && trMods["mods"].size() > 1 && trMods.value("total", 0) > 20;
				if (modsOk) modLabel = trMods["mods"][1].value("label", "");
				// its own search: a whole entry of the list matches at least itself
				modsOk = modsOk && !modLabel.empty()
				         && child.Call("trade_options_mods", json{{"prefix", "modSelector"}, {"query", modLabel}}, trModsQ, 60000)
				         && trModsQ.value("total", 0) >= 1 && trModsQ.value("total", 0) < trMods.value("total", 0);
				const int modIndex = modsOk ? trMods["mods"][1].value("index", 0) : 0;
				pickOk = modsOk && child.Call("trade_options_set",
				                              json{{"mod", json{{"prefix", "modSelector"}, {"row", 1}, {"sel", modIndex}}}}, trPick, 60000);
				bool picked = false;
				if (pickOk) for (auto& c : trPick["controls"])
					if (c.value("kind", "") == "mod" && c.value("row", 0) == 1 && c.value("label", "") == modLabel) picked = true;
				pickOk = pickOk && picked
				         && child.Call("trade_options_set", json{{"mod", json{{"prefix", "modSelector"}, {"row", 1}, {"min", "50"}}}}, trMin, 60000);
				bool minSet = false;
				if (pickOk) for (auto& c : trMin["controls"])
					if (c.value("kind", "") == "mod" && c.value("row", 0) == 1 && c.value("min", "") == "50") minSet = true;
				pickOk = pickOk && minSet;
				// Cancel takes the dialog back off POB's popup stack
				closedOk = child.Call("trade_options_cancel", json::object(), trClosed, 60000)
				           && !child.Call("trade_options_mods", json{{"prefix", "modSelector"}}, trClosedMods, 30000);
			}
			check(leagues ? "trade_options_open hands out POB's Query Options, its mod list searches, a filter sticks, and Cancel closes it"
			              : "trade_options_open needs a league list from the site; it was not reachable, so the dialog cannot be opened",
			      leagues ? (optOk && modsOk && pickOk && closedOk && child.Alive())
			              : (okTr && !tr0["rows"][0].value("canFindBest", true)),
			      optOk ? "controls=" + std::to_string(trOpt["controls"].size()) + " mods=" + std::to_string(trMods.value("total", 0))
			                  + " mod=" + modLabel.substr(0, 60)
			            : trOpt.dump().substr(0, 200));
			// the result tooltip is POB's own; with nothing searched it is refused
			check("trade_result_tooltip refuses a row that has no results yet",
			      !child.Call("trade_result_tooltip", json{{"row", 1}, {"index", 1}}, trTip, 30000) && child.Alive(),
			      trTip.dump().substr(0, 160));
			child.Call("trade_close", json::object(), trClose, 30000);
			bool rowsOk = okTr && tr0["rows"].size() >= 5 && tr0["rows"][0].contains("name") && tr0["rows"][0]["results"].is_array();
			check("trade_open lists POB's price-builder rows and settings; Price Item is refused without a login or URL",
			      rowsOk && okTrSet && trSet["tradeType"].value("sel", 0) == 2 && trSet.value("fetchPages", "") == "3" && !okBad && child.Alive(),
			      okTr ? "rows=" + std::to_string(tr0["rows"].size()) + " auth=" + std::to_string(tr0.value("authenticated", false)) + " " + tr0["rows"][0].dump().substr(0, 160)
			           : tr0.dump().substr(0, 220));
		}

		// --- graceful shutdown --------------------------------------------------
		child.Stop(10000);
		bool exited = child.WaitExit(0);
		check("child exits when the host closes stdin", exited && child.ExitCode() == 0,
		      "exit=" + std::to_string((long)child.ExitCode()));
		std::string stray = child.StrayOutput();
		if (!stray.empty()) line("INFO non-JSON output from child:\r\n" + stray.substr(0, 2000));

		// --- POB's own "basic" update, dry run ----------------------------------
		// A runtime-part file (lua\xml.lua) with a wrong sha1 forces "basic":
		// ApplyUpdate moves the program files in-process, rewrites the op list's
		// start line to this exe, drops the relaunch marker, spawns Update.exe
		// and Exit()s. POB_ZH_HEADLESS_DRYSPAWN keeps Update.exe from taking
		// over the sandbox; we run it ourselves afterwards without its start line.
		if (!untouchedNone) {
			check("basic update dry run", true, "skipped: the untouched sandbox already differs from upstream (a POB update is pending)");
		} else {
			const std::wstring rtFile = sandbox + L"\\lua\\xml.lua";
			const std::string manifest = ReadFileA(sandbox + L"\\manifest.xml");
			bool inManifest = manifest.find("name=\"lua/xml.lua\"") != std::string::npos;
			std::string orig = ReadFileA(rtFile);
			bool corrupted = inManifest && !orig.empty() && WriteFileA(rtFile, orig + corruption);
			const std::wstring markerPath = exeDir + L"pob-zh.relaunch";
			DeleteFileW(markerPath.c_str());
			SetEnvironmentVariableW(L"POB_ZH_HEADLESS_DRYSPAWN", L"1");
			HeadlessProc::Child b;
			std::string berr;
			json bh;
			bool spawned = corrupted && b.Start(opt, berr) && b.WaitEvent("hello", bh, 90000);
			SetEnvironmentVariableW(L"POB_ZH_HEADLESS_DRYSPAWN", nullptr);
			json u3;
			bool okU3 = spawned && b.Call("check_update_sync", json::object(), u3, 300000);
			check("a runtime file with the wrong sha1 makes the update check say \"basic\"",
			      okU3 && u3.value("mode", "") == "basic",
			      okU3 ? "mode=" + u3.value("mode", "") + " error=" + u3.value("error", "") : (corrupted ? berr + " " + u3.dump().substr(0, 200) : "lua/xml.lua not in manifest"));
			json ap3, evA, evS;
			bool basic = okU3 && u3.value("mode", "") == "basic";
			if (basic) b.Call("apply_update", json{{"mode", "basic"}}, ap3, 60000); // no reply: the child Exit()s inside the call
			bool gotApplying = basic && b.WaitEvent("update_applying", evA, 30000);
			bool gotSpawn = basic && b.WaitEvent("spawn_process", evS, 60000);
			bool bExited = basic && b.WaitExit(15000);
			std::wstring spawnPath = gotSpawn ? widen(evS.value("path", "")) : L"";
			check("apply_update{basic}: update_applying event, a dry spawn_process for Update, and the child exits 0 within 15 s",
			      gotApplying && gotSpawn && evS.value("dry", false) && spawnPath.size() >= 6 && spawnPath.compare(spawnPath.size() - 6, 6, L"Update") == 0 &&
			          bExited && b.ExitCode() == 0,
			      "applying=" + std::to_string(gotApplying) + " spawn=" + evS.dump().substr(0, 160) + " exited=" + std::to_string(bExited) + " code=" + std::to_string((long)b.ExitCode()));
			if (!bExited && spawned) b.Stop(0);
			// what the engine left for Update.exe and for the host
			wchar_t exeBuf[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
			std::string hostExe = narrow(exeBuf);
			for (auto& c : hostExe) if (c == '\\') c = '/';
			std::string ops = ReadFileA(sandbox + L"\\Update\\opFileRuntime.txt");
			std::string markerText = ReadFileA(markerPath);
			DeleteFileW(markerPath.c_str());
			PobLaunch::RelaunchMarker mk = PobLaunch::ParseRelaunchMarker(markerText);
			std::string sandboxLua = narrow(sandbox + L"\\Launch.lua");
			for (auto& c : sandboxLua) if (c == '\\') c = '/';
			check("opFileRuntime.txt ends by starting pob-zh.exe, and the relaunch marker names this Launch.lua with ui=modern",
			      basic && ops.find("start \"" + hostExe + "\"") != std::string::npos && mk.modern && narrow(mk.launchLua) == sandboxLua,
			      "ops_tail=" + (ops.size() > 160 ? ops.substr(ops.size() - 160) : ops) + " marker=" + markerText.substr(0, 200));
			// Update.exe itself, on the same op list minus the start line: the runtime file must come back.
			std::string opsNoStart;
			{
				size_t p = 0;
				while (p <= ops.size()) {
					size_t nl = ops.find('\n', p);
					std::string ln = ops.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
					p = nl == std::string::npos ? ops.size() + 1 : nl + 1;
					if (ln.rfind("start ", 0) != 0 && !ln.empty()) opsNoStart += ln + "\n";
				}
			}
			bool wroteOps = basic && WriteFileA(sandbox + L"\\Update\\opFileRuntime_test.txt", opsNoStart);
			unsigned long updExit = (unsigned long)-1;
			bool ranUpd = wroteOps && RunAndWait(sandbox + L"\\Update.exe", L"UpdateApply.lua Update/opFileRuntime_test.txt", sandbox, 120000, &updExit);
			std::string rtAfter = ReadFileA(rtFile);
			check("POB's Update.exe applies the runtime op list: exit 0 and lua\\xml.lua is POB's again",
			      ranUpd && updExit == 0 && !rtAfter.empty() && rtAfter.find(corruption) == std::string::npos,
			      "ran=" + std::to_string(ranUpd) + " exit=" + std::to_string((long)updExit) + " restored=" + std::to_string(rtAfter.find(corruption) == std::string::npos));
			if (corrupted && rtAfter.find(corruption) != std::string::npos) WriteFileA(rtFile, orig);
			// a fresh engine on the updated sandbox: manifest moved, nothing left to fetch
			HeadlessProc::Child c3;
			std::string c3err;
			json c3h, u4;
			bool okC3 = c3.Start(opt, c3err) && c3.WaitEvent("hello", c3h, 90000) && c3.Call("check_update_sync", json::object(), u4, 300000);
			check("after the basic update the sandbox is current again (a fresh engine says \"none\")",
			      okC3 && u4.value("mode", "") == "none", okC3 ? "mode=" + u4.value("mode", "") + " err=" + u4.value("error", "") : c3err + " " + u4.dump().substr(0, 200));
			c3.Stop(10000);
			Rmtree(sandbox + L"\\Update");
		}

		// --- a bridge whose probe no longer matches POB: the gate must say so --
		// (what a POB update that renames a function looks like; the window
		// falls back to classic on this verdict and the launcher greys its button)
		{
			std::wstring bridgeSrc = opt.bridgeLua.empty() ? exeDir + L"Data\\bridge\\bridge.lua" : opt.bridgeLua;
			std::string src = ReadFileA(bridgeSrc);
			const std::string needle = "type(launch.OnFrame) == \"function\"";
			size_t at = src.find(needle);
			const std::wstring brokenBridge = sandboxRoot + L"\\broken_bridge.lua";
			bool made = at != std::string::npos && WriteFileA(brokenBridge, src.substr(0, at) + "type(launch.OnFrameNoSuchThing) == \"function\"" + src.substr(at + needle.size()));
			HeadlessProc::Options gopt = opt;
			gopt.bridgeLua = brokenBridge;
			HeadlessProc::Child g;
			std::string gerr;
			json ggate;
			bool gspawned = made && g.Start(gopt, gerr);
			bool gotGate2 = gspawned && g.WaitEvent("gate_result", ggate, 90000);
			bool named = false;
			if (gotGate2) for (auto& f : ggate.value("failed", json::array())) if (f.is_string() && f.get<std::string>().find("launch.OnFrame") != std::string::npos) named = true;
			check("a probe that no longer matches POB fails the gate, naming the probe and the POB version",
			      gotGate2 && !ggate.value("ok", true) && named && !ggate.value("pobVersion", "").empty(),
			      gotGate2 ? ggate.dump().substr(0, 300) : (made ? gerr : "could not derive a broken bridge from " + narrow(bridgeSrc)));
			if (gspawned) g.Stop(5000);
			DeleteFileW(brokenBridge.c_str());
		}

		// --- a broken script must end the child, not hang it --------------------
		{
			const std::wstring broken = sandboxRoot + L"\\broken";
			Rmtree(broken);
			MkdirP(broken);
			WriteFileA(broken + L"\\Launch.lua", "this is not lua (\n");
			HeadlessProc::Options bopt = opt;
			bopt.launchLua = broken + L"\\Launch.lua";
			HeadlessProc::Child b;
			std::string berr;
			bool spawned = b.Start(bopt, berr);
			bool bexit = spawned && b.WaitExit(15000);
			json ev;
			bool gotErr = spawned && b.WaitEvent("error", ev, 1000);
			check("broken Launch.lua: child exits by itself within 15 s", bexit,
			      spawned ? "exit=" + std::to_string((long)b.ExitCode()) : berr);
			check("broken Launch.lua: exit code is non-zero", bexit && b.ExitCode() != 0,
			      "exit=" + std::to_string((long)b.ExitCode()));
			check("broken Launch.lua: an error event names the problem", gotErr, gotErr ? ev.dump().substr(0, 300) : "");
			if (!bexit) b.Stop(0);
			Rmtree(broken);
		}
	}

done:
	line("PASS " + std::to_string(g_pass) + "   FAIL " + std::to_string(g_fail));
	line(g_fail == 0 ? "ALL PASS" : "FAILURES");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", g_rep.c_str());
	WriteFileA(reportPath, g_rep);
	return g_fail;
}

// --headless-selftest-poe2 [pobDir]: the same engine and bridge against PoE2's
// Path of Building (a fork with its own tree art, classes, weapon-set passives
// and attribute nodes, and without some PoE1 surfaces). A PoE2 install ships no
// builds, so the test makes one through the bridge and uses POB's own save of
// it as the oracle. Report at <exeDir>headless_selftest_poe2.txt.
int RunHeadlessSelfTestPoe2(const std::wstring& exeDir, const std::wstring& pobDirOverride)
{
	g_rep.clear(); g_fail = 0; g_pass = 0;
	const std::wstring reportPath = exeDir + L"headless_selftest_poe2.txt";
	DeleteFileW(reportPath.c_str());
	line("PobTools --headless-selftest-poe2");

	std::wstring pobDir = pobDirOverride;
	if (pobDir.empty()) {
		InstallInfo info = DetectInstalls(exeDir);
		pobDir = info.poe2Dir;
	}
	while (!pobDir.empty() && pobDir.back() == L'\\') pobDir.pop_back();
	check("PoE2 install found", !pobDir.empty() && IsDir(pobDir), narrow(pobDir));
	if (pobDir.empty() || !IsDir(pobDir)) {
		line("no PoE2 Path of Building install beside the exe; pass its folder as the second argument");
	} else {
		const std::wstring sandboxRoot = exeDir + L"PobTools\\sandbox";
		const std::wstring sandbox = sandboxRoot + L"\\PathOfBuildingCommunity-PoE2";
		Rmtree(sandbox);
		MkdirP(sandboxRoot);
		CopyStats st;
		CopyTree(pobDir, sandbox, L"", st);
		MkdirP(sandbox + L"\\Builds");
		check("sandbox copied (TreeData junctioned; copied under Wine)", st.errors == 0 && st.files > 50 &&
		      (IsReparse(sandbox + L"\\TreeData") || (PobLaunch::RunningUnderWine() && IsDir(sandbox + L"\\TreeData"))),
		      "files=" + std::to_string(st.files) + " errors=" + std::to_string(st.errors));
		const std::string manifestVersion = ManifestVersion(ReadFileA(sandbox + L"\\manifest.xml"));

		HeadlessProc::Options opt;
		opt.exeDir = exeDir;
		opt.launchLua = sandbox + L"\\Launch.lua";
		opt.game = L"poe2";
		opt.locale = L"zh-rTW";
		{
			wchar_t env[2048] = {};
			if (GetEnvironmentVariableW(L"POB_ZH_BRIDGE", env, 2048)) opt.bridgeLua = env;
		}
		HeadlessProc::Child child;
		std::string err;
		check("headless child spawned", child.Start(opt, err), err);
		json hello, gate, ver;
		bool gotHello = child.WaitEvent("hello", hello, 90000);
		check("hello event within 90 s, POB version = manifest", gotHello && hello.value("pobVersion", "") == manifestVersion,
		      gotHello ? hello.value("pobVersion", "") + " vs " + manifestVersion : child.StrayOutput());
		bool gotGate = child.WaitEvent("gate_result", gate, 5000);
		check("compatibility gate ok on PoE2's POB (PoE1-only surfaces are capabilities, not failures)",
		      gotGate && gate.value("ok", false), gotGate ? gate.dump().substr(0, 600) : "");
		bool okVer = child.Call("version", json::object(), ver, 30000);
		check("version names the game poe2", okVer && ver.value("game", "") == "poe2", ver.dump().substr(0, 200));
		if (okVer) {
			const json& caps = ver["caps"];
			check("capabilities switched off where PoE2's POB has no such surface (influence, enchant, crucible, account-name import)",
			      caps.value("itemInfluence", true) == false && caps.value("itemEnchant", true) == false &&
			          caps.value("itemCrucible", true) == false && caps.value("siteImport", true) == false,
			      caps.dump());
		}

		// --- a new build ---------------------------------------------------------
		json nb, cls;
		bool okNew = child.Call("new_build", json::object(), nb, 120000);
		check("new_build", okNew, nb.dump().substr(0, 200));
		bool okCls = child.Call("list_classes", json::object(), cls, 30000);
		size_t nCls = okCls ? cls["classes"].size() : 0;
		bool ascOk = nCls > 0;
		for (size_t i = 0; okCls && i < nCls; i++) if (cls["classes"][i]["ascendancies"].size() < 2) ascOk = false;
		check("list_classes: PoE2's classes, each with ascendancies after None", nCls >= 8 && ascOk, "classes=" + std::to_string(nCls));

		// --- tree ------------------------------------------------------------------
		json td, ta, ts;
		bool okTs = child.Call("get_tree_state", json::object(), ts, 30000);
		std::string version = okTs ? ts.value("treeVersion", "") : "";
		bool okTd = !version.empty() && child.Call("tree_data", json{{"version", version}}, td, 120000);
		int nodeCount = okTd ? td.value("nodeCount", 0) : 0;
		int arcsWithCentre = 0, sized = 0;
		if (okTd) {
			for (auto& c : td["connectors"]) if (c.contains("orbit") && c.contains("cx")) arcsWithCentre++;
			for (auto& [k, n] : td["nodes"].items()) if (n.contains("draw")) sized++;
		}
		check("tree_data: POB laid out the PoE2 tree (>3000 nodes, target sizes, arc centres from BuildArc)",
		      okTd && nodeCount > 3000 && sized * 10 >= nodeCount * 9 && arcsWithCentre > 100,
		      "nodes=" + std::to_string(nodeCount) + " sized=" + std::to_string(sized) + " arcs=" + std::to_string(arcsWithCentre));
		bool classBg = false;
		if (okTd) for (auto& c : td["classes"]) if (c.contains("background") && !c["background"]["ascendancies"].empty()) classBg = true;
		check("tree_data: class plates and ascendancy plates named", classBg);

		ULONGLONG t0 = GetTickCount64();
		bool okTa = !version.empty() && child.Call("tree_assets", json{{"version", version}}, ta, 300000);
		ULONGLONG firstMs = GetTickCount64() - t0;
		int cacheRects = 0;
		std::string oneCacheFile;
		if (okTa) {
			for (auto& [k, r] : ta["assets"].items()) {
				std::string f = r.value("file", "");
				if (f.rfind("cache:", 0) == 0) { cacheRects++; if (oneCacheFile.empty()) oneCacheFile = f.substr(6); }
			}
		}
		std::wstring onDisk = exeDir + L"PobTools\\cache\\" + widen(oneCacheFile);
		for (auto& ch : onDisk) if (ch == L'/') ch = L'\\';
		check("tree_assets: every DDS array decoded into cached PNG pages (no missing sheets)",
		      okTa && ta["missingSheets"].empty() && cacheRects > 500 && GetFileAttributesW(onDisk.c_str()) != INVALID_FILE_ATTRIBUTES,
		      "cacheRects=" + std::to_string(cacheRects) + " ms=" + std::to_string(firstMs) + " missing=" + (okTa ? ta["missingSheets"].dump().substr(0, 200) : ta.dump().substr(0, 200)));
		t0 = GetTickCount64();
		json ta2;
		bool okTa2 = child.Call("tree_assets", json{{"version", version}}, ta2, 60000);
		ULONGLONG secondMs = GetTickCount64() - t0;
		check("tree_assets again answers from the cache (unchanged art is not decoded twice)", okTa2 && secondMs < 5000 && secondMs * 3 < firstMs + 3000,
		      "first=" + std::to_string(firstMs) + "ms second=" + std::to_string(secondMs) + "ms");

		// A path from the class start: breadth-first over POB's links, normal
		// nodes only, the first few in order so every click is adjacent.
		int startId = 0;
		if (okTd && okTs) {
			for (auto& c : td["classes"]) if (c.value("name", "") == ts.value("className", "")) startId = c.value("startNodeId", 0);
		}
		std::vector<int> pathIds;
		int attrId = 0;
		if (startId && okTd) {
			std::set<int> seen{ startId };
			std::vector<int> queue{ startId };
			for (size_t qi = 0; qi < queue.size() && pathIds.size() < 12; qi++) {
				const json& cur = td["nodes"][std::to_string(queue[qi])];
				for (auto& nb2 : cur["linked"]) {
					int id = nb2.get<int>();
					const std::string key = std::to_string(id);
					if (seen.count(id) || !td["nodes"].contains(key)) continue;
					const json& n = td["nodes"][key];
					if (n.contains("asc") || (n.value("type", "") != "Normal" && n.value("type", "") != "Notable")) continue;
					seen.insert(id);
					queue.push_back(id);
					pathIds.push_back(id);
					if (!attrId && n.value("attribute", false)) attrId = id;
				}
			}
		}
		int allocated = okTs ? ts.value("allocCount", 0) : 0;
		int asked = 0, clicked = 0;
		bool attrOverride = false;
		for (int id : pathIds) {
			json r;
			if (!child.Call("tree_click", json{{"id", id}}, r, 60000)) break;
			if (r.value("needsAttribute", false)) {
				asked++;
				if (!child.Call("tree_click", json{{"id", id}, {"attribute", 2}}, r, 60000)) break;
				if (r["overrides"].contains(std::to_string(id)) && r["overrides"][std::to_string(id)].value("why", "") == "attribute") attrOverride = true;
			}
			if (r.value("allocCount", 0) == allocated + 1) { allocated++; clicked++; }
		}
		check("tree_click allocates a path from the class start, one node per click",
		      !pathIds.empty() && clicked == (int)pathIds.size(),
		      "path=" + std::to_string(pathIds.size()) + " allocated=" + std::to_string(clicked));
		check("attribute nodes ask which attribute (ModifyAttributePopup) and the choice sticks (hashOverrides)",
		      attrId == 0 || (asked > 0 && attrOverride), "asked=" + std::to_string(asked) + " attrId=" + std::to_string(attrId));
		if (attrId) {
			json sw;
			bool okSw = child.Call("tree_attribute", json{{"id", attrId}, {"attribute", 3}}, sw, 60000);
			const std::string key = std::to_string(attrId);
			check("tree_attribute switches an allocated attribute node without deallocating it",
			      okSw && sw.value("allocCount", 0) == allocated && sw["overrides"].contains(key) &&
			          sw["overrides"][key].value("name", "") != "",
			      okSw ? sw["overrides"].value(key, json::object()).dump().substr(0, 200) : sw.dump().substr(0, 200));
		}

		// weapon-set allocation
		json m1;
		bool okM1 = child.Call("set_alloc_mode", json{{"mode", 1}}, m1, 60000);
		int setNode = 0;
		if (okM1 && startId && okTd) {
			// any unallocated neighbour of an allocated normal node
			std::set<int> alloc;
			for (auto& a : m1["allocatedNodes"]) alloc.insert(a.get<int>());
			for (int id : pathIds) {
				for (auto& nb2 : td["nodes"][std::to_string(id)]["linked"]) {
					int o = nb2.get<int>();
					const std::string key = std::to_string(o);
					if (alloc.count(o) || !td["nodes"].contains(key)) continue;
					const json& n = td["nodes"][key];
					if (n.value("type", "") == "Normal" && !n.contains("asc") && !n.value("attribute", false)) { setNode = o; break; }
				}
				if (setNode) break;
			}
		}
		json sc;
		bool okSc = setNode && child.Call("tree_click", json{{"id", setNode}}, sc, 60000);
		check("set_alloc_mode 1 then a click: the node is allocated to weapon set 1 (nodeModes)",
		      okM1 && m1.value("allocMode", -1) == 1 && okSc && sc["nodeModes"].value(std::to_string(setNode), 0) == 1,
		      "node=" + std::to_string(setNode) + " " + (okSc ? sc["nodeModes"].dump() : sc.dump()).substr(0, 200));
		// still in weapon-set mode: a keystone is global and stays on the main
		// tree (PassiveTreeView's shouldBlockGlobalNodeAllocation), so the click
		// is refused and nothing is allocated
		int keystone = 0;
		if (okTd) for (auto& [k, n] : td["nodes"].items()) if (n.value("type", "") == "Keystone" && !n.contains("asc")) { keystone = n.value("id", 0); break; }
		json kb;
		bool okKb = keystone && okSc && child.Call("tree_click", json{{"id", keystone}}, kb, 60000);
		check("weapon-set mode refuses a keystone (blocked: weapon_set_global) and allocates nothing",
		      okKb && kb.value("blocked", "") == "weapon_set_global", kb.dump().substr(0, 200));
		json m0, undo;
		child.Call("set_alloc_mode", json{{"mode", 0}}, m0, 60000);
		bool okUndo = okSc && child.Call("tree_undo", json::object(), undo, 60000);
		check("tree_undo takes the weapon-set node back", okUndo && undo.value("allocCount", -1) == allocated,
		      okUndo ? std::to_string(undo.value("allocCount", -1)) + " vs " + std::to_string(allocated) : undo.dump().substr(0, 200));

		// --- items and skills ----------------------------------------------------
		const std::string bow = "Rarity: Rare\nStorm Thirst\nRecurve Bow\n--------\nItem Level: 80\n--------\n"
		                        "Adds 20 to 60 Lightning Damage\n+120 to Accuracy Rating\n15% increased Attack Speed\n";
		json ai, ag, stats;
		bool okAi = child.Call("add_item", json{{"raw", bow}, {"slotName", "Weapon 1"}, {"equip", true}}, ai, 60000);
		check("add_item parses a PoE2 bow and equips it", okAi && ai["item"].value("baseName", "") == "Recurve Bow", ai.dump().substr(0, 300));
		bool okAg = child.Call("add_group", json{{"slot", "Weapon 1"}, {"gems", json::array({ json{{"nameSpec", "Lightning Arrow"}} })}}, ag, 60000);
		bool okSt = okAg && child.Call("get_stats", json::object(), stats, 30000);
		double avg = okSt ? stats["stats"].value("AverageDamage", 0.0) : 0.0;
		check("add_group Lightning Arrow on the bow: POB calculates damage", okAg && avg > 0, "AverageDamage=" + std::to_string(avg));

		int bowId = okAi ? ai["item"].value("id", 0) : 0;
		json tt;
		bool okTt = bowId && child.Call("item_tooltip", json{{"id", bowId}}, tt, 60000);
		int zhLines = 0;
		if (okTt) for (auto& l : tt["lines"]) if (l.contains("text") && l.value("text", "") != l.value("raw", "")) zhLines++;
		check("item_tooltip: POB's tooltip for the bow, translated", okTt && tt["lines"].size() > 5 && zhLines > 0,
		      "lines=" + std::to_string(okTt ? tt["lines"].size() : 0) + " zh=" + std::to_string(zhLines));

		json eb, es, ec, rawAfter;
		bool okEb = bowId && child.Call("item_edit_begin", json{{"id", bowId}}, eb, 60000);
		bool okEs = okEb && child.Call("item_edit_set", json{{"quality", 20}}, es, 60000);
		bool q20 = false;
		if (okEs) for (auto& l : es["tooltip"]["lines"]) if (l.value("raw", "").find("20%") != std::string::npos) q20 = true;
		bool okEc = okEs && child.Call("item_edit_cancel", json::object(), ec, 60000);
		bool okRaw = okEc && child.Call("item_raw", json{{"id", bowId}}, rawAfter, 30000);
		check("item editing on PoE2: quality 20 shows in the edit tooltip, cancel leaves the build's bow unchanged",
		      okEs && q20 && okRaw && rawAfter.value("raw", "").find("Quality: +20%") == std::string::npos,
		      okEs ? "" : es.dump().substr(0, 300));

		json dbo;
		bool okDbo = child.Call("item_db_options", json{{"kind", "unique"}}, dbo, 180000);
		check("item_db_options: PoE2's unique database filters", okDbo && dbo.contains("slot") && dbo["slot"].size() > 5, dbo.dump().substr(0, 200));

		// --- a real zh-TW PoE2 copy through the paste path ------------------------
		{
			const PoE2PasteFixture* wand = nullptr;
			for (const PoE2PasteFixture& fx : kPoe2PasteFixtures) if (std::string(fx.name) == "wand-rare-dueling-wand-01") wand = &fx;
			json pt;
			bool okPt = wand && child.Call("parse_item_text", json{{"raw", wand->text}}, pt, 60000);
			int unsupported = 0;
			std::string unsupportedText;
			// Only lines the paste actually carries count. The wand names no rune, so
			// POB infers one from the "(rune)" line (Hedgewitch Assandra's Rune of
			// Wisdom) and adds that rune's Bonded line itself -- a stat POB has no
			// parser for. That line is POB's own guess, not text we translated.
			const std::string pastedText = okPt ? pt.value("text", "") : "";
			if (okPt) for (auto& l : pt["lines"]) if (l.value("unsupported", false) && pastedText.find(l.value("line", "")) != std::string::npos) { unsupported++; unsupportedText += " [" + l.dump().substr(0, 160) + "]"; }
			check("parse_item_text: a zh-TW PoE2 wand comes back in English, POB reads its base and every line",
			      okPt && pt.value("reversed", false) && pt.value("parsed", false) && pt["untranslated"].empty() &&
			          pt["lines"].size() >= 7 && unsupported == 0,
			      okPt ? "base=" + pt.value("baseName", "") + " lines=" + std::to_string(pt["lines"].size()) +
			                 " untranslated=" + pt["untranslated"].dump().substr(0, 200) + " unsupported=" + std::to_string(unsupported) + unsupportedText
			           : pt.dump().substr(0, 200));
		}

		// --- a trade-site paste: the "Requirements:" block ------------------------
		// PoE1's POB consumes "Dex: 99" into the item's requirements; the PoE2
		// fork kept the header and the Level line but never ported the attribute
		// branch, so the line fell through to the mod parser and the item wore a
		// modifier reading "Dex: 99 (Not supported in PoB yet)". Dropped in
		// poecharm_inject.lua -- POB works the requirement out from the base and
		// the mods anyway, which is what PoE1 ends up showing.
		{
			const char* kTradePaste =
			    "Rarity: Rare\nPandemonium Star\nDesert Cap\n--------\nHelmet\n"
			    "Quality: +20% (augmented)\nEvasion Rating: 806 (augmented)\n--------\n"
			    "Requirements:\nLevel: 70\nDex: 99\n--------\nSockets: S \n--------\n"
			    "Item Level: 80\n--------\n+183 to Evasion Rating\n+65 to maximum Mana\n"
			    "+30% to Cold Resistance\n+32% to Lightning Resistance\n"
			    "Gain Deflection Rating equal to 21% of Evasion Rating\n"
			    "36% increased Evasion Rating\n+39 to maximum Life\n--------\nNote: ~b/o 2 chaos\n";
			json tp, tpCancel;
			bool okTp = child.Call("item_edit_begin", json{{"raw", kTradePaste}}, tp, 60000);
			int mods = 0;
			bool sawAttrLine = false;
			if (okTp) {
				for (auto& m : tp["modLines"]) {
					mods++;
					if (m.value("text", "").find("Dex:") != std::string::npos) sawAttrLine = true;
				}
			}
			check("a trade-site paste's \"Requirements:\" block does not turn Dex/Str/Int into a modifier",
			      okTp && mods == 7 && !sawAttrLine && tp["summary"].value("baseName", "") == "Desert Cap",
			      okTp ? "mods=" + std::to_string(mods) + " attrLine=" + std::to_string(sawAttrLine) +
			                 " base=" + tp["summary"].value("baseName", "")
			           : tp.dump().substr(0, 300));
			// The helmet has one augment socket: the panel must offer POB's "Rune #1"
			// drop-down (field-reported missing from the new UI), and picking a rune
			// must put its line into the item under the "rune" section.
			if (okTp) {
				int slots = tp.contains("runeSlots") ? (int)tp["runeSlots"].size() : 0;
				int pick = 0;
				if (slots == 1)
					for (size_t k = 0; k < tp["runeSlots"][0]["options"].size(); k++)
						if (tp["runeSlots"][0]["options"][k].value("name", "") == "Greater Glacial Rune") pick = (int)k + 1;
				json rs;
				bool okRs = pick > 0 && child.Call("item_edit_set", json{{"rune", {{"index", 1}, {"sel", pick}}}}, rs, 60000);
				int runeLines = 0;
				std::string runeText;
				if (okRs)
					for (auto& m : rs["modLines"])
						if (m.value("section", "") == "rune") { runeLines++; runeText = m.value("text", ""); }
				check("PoE2 augment sockets: the Rune #1 drop-down is offered and picking a rune adds its line",
				      okRs && rs["runeSlots"][0].value("sel", 0) == pick && runeLines >= 1 &&
				          runeText.find("Cold Resistance") != std::string::npos,
				      "slots=" + std::to_string(slots) + " pick=" + std::to_string(pick) +
				          " runeLines=" + std::to_string(runeLines) + " text=" + runeText);
			}
			child.Call("item_edit_cancel", json::object(), tpCancel, 60000);
		}

		// --- a rune that changes the affix limit ------------------------------------
		// Field report: craft a weapon, pick "+1 Suffix Modifier allowed" (Serle's
		// Triumph) and merely hovering the new suffix row crashed POB -- the rune's
		// selFunc never called UpdateAffixControls, so that row's list was empty
		// (upstream master 0.23.1, English too; poecharm_inject PATCHES["ItemsTab"]).
		// Every affix row shown after the pick must have a list its selection is in.
		{
			json co, ed, rs, cancel;
			std::string type, base;
			if (child.Call("craft_item_options", json::object(), co, 60000))
				for (auto& ty : co["types"]) {
					std::string tn = ty.value("type", "");
					if ((tn == "Staff" || tn == "Quarterstaff" || tn == "Two Handed Mace") && ty["bases"].size()) {
						type = tn;
						base = ty["bases"][ty["bases"].size() - 1].value("name", "");
						break;
					}
				}
			bool okEd = !type.empty() && child.Call("item_edit_begin", json{{"craft", json{{"rarity", "RARE"}, {"type", type}, {"base", base}}}}, ed, 60000);
			int pick = 0, before = okEd ? (int)ed["affixes"].size() : 0;
			if (okEd && ed.contains("runeSlots") && ed["runeSlots"].size())
				for (size_t k = 0; k < ed["runeSlots"][0]["options"].size(); k++)
					if (ed["runeSlots"][0]["options"][k].value("name", "") == "Serle's Triumph") pick = (int)k + 1;
			bool okRs = pick > 0 && child.Call("item_edit_set", json{{"rune", {{"index", 1}, {"sel", pick}}}}, rs, 60000);
			int after = okRs ? (int)rs["affixes"].size() : 0, broken = 0;
			if (okRs)
				for (auto& a : rs["affixes"])
					if (a["options"].empty() || a.value("sel", 0) < 1 || a.value("sel", 0) > (int)a["options"].size()) broken++;
			check("PoE2: a rune that allows one more suffix gives the new row a real list (no crash on hover)",
			      okRs && after == before + 1 && broken == 0,
			      "type=" + type + "/" + base + " pick=" + std::to_string(pick) + " rows " + std::to_string(before) + "->" +
			          std::to_string(after) + " broken=" + std::to_string(broken));
			child.Call("item_edit_cancel", json::object(), cancel, 60000);
		}

		// --- config, calcs, notes --------------------------------------------------
		json lc, gc;
		bool okLc = child.Call("list_config", json::object(), lc, 60000);
		std::string checkVar;
		if (okLc) {
			for (auto& s : lc["sections"]) {
				for (auto& it : s["items"]) {
					if (it.value("type", "") == "check" && it.value("visible", false)) { checkVar = it.value("var", ""); break; }
				}
				if (!checkVar.empty()) break;
			}
		}
		json sc1, rc1;
		bool okSc1 = !checkVar.empty() && child.Call("set_config", json{{"var", checkVar}, {"value", true}}, sc1, 60000);
		bool okRc1 = okSc1 && child.Call("reset_config", json{{"var", checkVar}}, rc1, 60000);
		check("list_config: PoE2's ConfigOptions sections; a checkbox sets and resets", okLc && lc["sections"].size() > 3 && okRc1,
		      "var=" + checkVar);
		bool okGc = child.Call("get_calcs", json::object(), gc, 60000);
		check("get_calcs: PoE2's CalcSections laid out", okGc && gc["sections"].size() > 10, okGc ? "" : gc.dump().substr(0, 200));

		// --- share code and save oracle --------------------------------------------
		json ex, dec;
		bool okEx = child.Call("export_code", json::object(), ex, 60000);
		bool okDec = okEx && child.Call("decode_code", json{{"code", ex.value("code", "")}}, dec, 60000);
		check("export_code -> decode_code on a PoE2 build", okDec, dec.dump().substr(0, 200));
		json imp, stImp;
		bool okImp = okDec && child.Call("import_code", json{{"code", ex.value("code", "")}, {"mode", "replace"}}, imp, 180000);
		bool okStImp = okImp && child.Call("get_stats", json::object(), stImp, 30000);
		double avgImp = okStImp ? stImp["stats"].value("AverageDamage", 0.0) : -1;
		check("import_code{replace} of the build's own code gives the same AverageDamage",
		      okStImp && std::fabs(avgImp - avg) <= 1e-9 * std::fmax(1.0, std::fabs(avg)),
		      std::to_string(avgImp) + " vs " + std::to_string(avg) + (okImp ? "" : " " + imp.dump().substr(0, 200)));

		// PoE2's tree list: its PassiveSpecListControl and reset dialog (no links, no tattoos)
		{
			json ls0, cp, dl, rs, ls1, lnk;
			bool okLs = child.Call("list_specs", json::object(), ls0, 30000);
			const size_t n0 = okLs ? ls0["specs"].size() : 0;
			const int act0 = okLs ? ls0.value("activeSpec", 1) : 1;
			const int pts0 = (okLs && n0 > 0) ? ls0["specs"][act0 - 1].value("points", -1) : -1;
			bool okCp = okLs && child.Call("spec_op", json{{"op", "copy"}, {"index", act0}, {"title", "poe2 copy"}}, cp, 60000);
			bool okRs = okCp && child.Call("set_active_spec", json{{"index", (int)n0 + 1}}, ls1, 60000) && child.Call("reset_tree", json::object(), rs, 60000);
			json after;
			bool okAfter = okRs && child.Call("list_specs", json::object(), after, 30000);
			bool okDl = okAfter && child.Call("spec_op", json{{"op", "delete"}, {"index", (int)n0 + 1}}, dl, 60000);
			bool okLnk = child.Call("export_tree_url", json::object(), lnk, 30000);
			check("PoE2: list_specs/spec_op copy/set_active_spec/reset_tree/delete through its own list control; tree links are refused",
			      okDl && !ls0.value("treeLinks", true) && !ls0.value("tattoos", true) && cp["specs"].size() == n0 + 1 &&
			          after["specs"][n0].value("points", -1) == 0 && after["specs"][act0 - 1].value("points", -1) == pts0 &&
			          dl["specs"].size() == n0 && !okLnk && child.Alive(),
			      "pts0=" + std::to_string(pts0) + " " + (okAfter ? after.dump().substr(0, 200) : (cp.dump() + rs.dump()).substr(0, 300)));
			child.Call("set_active_spec", json{{"index", act0}}, ls1, 60000);
		}

		json ver2, saveAs, lb, loaded, stats2, saved;
		child.Call("version", json::object(), ver2, 30000);
		const std::string buildPath = ver2.value("buildPath", "");
		const std::string file = buildPath + "selftest_poe2.xml";
		bool okSaveAs = !buildPath.empty() && child.Call("save_build_as", json{{"path", file}}, saveAs, 60000);
		bool okLb = okSaveAs && child.Call("list_builds", json::object(), lb, 30000);
		bool listed = false;
		if (okLb) for (auto& e : lb["entries"]) if (e.value("fileName", "") == "selftest_poe2.xml" || e.value("buildName", "") == "selftest_poe2") listed = true;
		check("save_build_as then list_builds finds it (PoE2's BuildListHelpers)", okSaveAs && listed, lb.dump().substr(0, 300));
		bool okLoad = okSaveAs && child.Call("load_build_file", json{{"path", file}}, loaded, 120000);
		bool okSt2 = okLoad && child.Call("get_stats", json::object(), stats2, 30000);
		bool okSave = okSt2 && child.Call("save_xml", json::object(), saved, 60000);
		if (okSave) {
			auto oracle = ParsePlayerStats(saved.value("xml", ""));
			int missing = 0;
			std::vector<std::string> exs;
			int bad = CompareStats(oracle, stats2["stats"], 1e-9, missing, exs);
			std::string detail = "checked=" + std::to_string(oracle.size()) + " bad=" + std::to_string(bad) + " missing=" + std::to_string(missing);
			for (auto& e : exs) detail += "; " + e;
			check("hard oracle after reload: every <PlayerStat> POB saves equals get_stats (rel 1e-9)",
			      oracle.size() > 20 && bad == 0 && missing == 0, detail);
		} else {
			check("hard oracle after reload: every <PlayerStat> POB saves equals get_stats (rel 1e-9)", false, loaded.dump().substr(0, 300));
		}
		json allocAfter;
		bool okAa = okLoad && child.Call("get_tree_state", json::object(), allocAfter, 30000);
		check("the reloaded build keeps its allocation and attribute choices", okAa && allocAfter.value("allocCount", 0) == allocated &&
		      (attrId == 0 || allocAfter["overrides"].contains(std::to_string(attrId))),
		      okAa ? std::to_string(allocAfter.value("allocCount", 0)) + " vs " + std::to_string(allocated) : "");

		// --- F2's switch on PoE2's POB too ---------------------------------------
		// Same flag, same bridge code, different dictionaries: worth its own
		// check because this is the game where "it only works on PoE1" hides.
		{
			json off, on, sideOff, sideOn, ver;
			const bool okOff = child.Call("set_translate", json{{"enabled", false}}, off, 30000) &&
			                   child.Call("get_sidebar", json::object(), sideOff, 30000);
			int offText = 0, offSame = 0;
			if (okOff)
				for (auto& r : sideOff["rows"])
					if (r.contains("lhs")) {
						offText++;
						if (r.value("lhs", "") == r.value("lhsRaw", "")) offSame++;
					}
			const bool okVer = child.Call("version", json::object(), ver, 30000);
			const bool okOn = child.Call("set_translate", json{{"enabled", true}}, on, 30000) &&
			                  child.Call("get_sidebar", json::object(), sideOn, 30000);
			int onZh = 0;
			if (okOn)
				for (auto& r : sideOn["rows"])
					if (r.contains("lhs") && r.value("lhs", "") != r.value("lhsRaw", "")) onZh++;
			check("PoE2: set_translate turns the sidebar back to POB's English and returns",
			      okOff && offText > 5 && offSame == offText && okVer &&
			          ver["caps"].value("translateToggle", false) && ver.value("translate", true) == false &&
			          okOn && onZh > 0,
			      "off " + std::to_string(offSame) + "/" + std::to_string(offText) + ", on " + std::to_string(onZh));
		}

		// --- import ------------------------------------------------------------------
		json is, fc;
		bool okIs = child.Call("import_status", json::object(), is, 60000);
		bool poe2Realm = false;
		if (okIs) for (auto& r : is["realms"]) if (r.value("realmCode", "") == "poe2") poe2Realm = true;
		check("import_status lists the PoE2 realm", okIs && poe2Realm, is.value("realms", json::array()).dump());
		bool okFc = child.Call("fetch_characters", json{{"source", "site"}, {"realm", "PoE2"}, {"accountName", "Someone#1234"}}, fc, 30000);
		check("fetch_characters{site} is refused on PoE2's POB (no account-name import) and the child survives",
		      !okFc && child.Alive(), fc.dump().substr(0, 200));

		// PoE2's ImportTab runs the whole import itself, in a shape that has
		// nothing in common with PoE1's: a raw JSON body, its own controls, its
		// own status line. The bridge must report that it took that path, or it
		// is back to driving PoE1's fields on a tab that does not have them --
		// which is exactly how the character list stayed empty forever.
		check("import_status reports PoE2's self-driving import shape",
		      okIs && is.value("selfDriving", false) && is["site"].contains("mode"),
		      okIs ? ("selfDriving=" + std::to_string(is.value("selfDriving", false)) +
		              " mode=" + is["site"].value("mode", "?")) : "");
		// Nothing was fetched, so there is no character to import: the refusal
		// must name the fetch, not blow up on a nil list.
		json impRes;
		const bool okAcctImp = child.Call("import_account_character",
		                              json{{"source", "oauth"}, {"realm", "PoE2"}, {"name", "Nobody"}, {"what", "tree"}},
		                              impRes, 30000);
		const std::string impMsg = okAcctImp ? "" : impRes.value("message", impRes.dump());
		check("import_account_character on PoE2 refuses cleanly before any download",
		      !okAcctImp && child.Alive() &&
		          (impMsg.find("fetch the character list first") != std::string::npos ||
		           impMsg.find("not in the fetched list") != std::string::npos),
		      impMsg.substr(0, 160));
		// pump is what lets a caller give POB's own frame loop time inside one
		// call; every async import/trade test leans on it.
		json pumped;
		const bool okPump = child.Call("pump", json{{"seconds", 1}}, pumped, 30000);
		check("pump runs POB's frame loop for the seconds asked",
		      okPump && pumped.value("frames", 0) > 0 && pumped.value("seconds", 0) == 1,
		      pumped.dump().substr(0, 120));

		child.Stop(5000);

		// --- the system-browser fallback (what Wine / CrossOver get) -------------
		// The loopback server against this sandbox: the page with its boot
		// script, the folder mounts, the refusals (token, climbing, Host), the
		// event stream carrying the engine's hello, a host.* round trip and
		// host.close ending it.
		{
			SetEnvironmentVariableW(L"POB_ZH_NO_BROWSER", L"1");
			const std::wstring urlFile = exeDir + L"PobTools\\modern_ui_url_poe2.txt";
			DeleteFileW(urlFile.c_str());
			LauncherConfig bcfg = LoadLauncherConfig(exeDir + L"pob-zh.ini");
			std::atomic<int> brc{ -1000 };
			std::thread server([&]() { brc = ShowModernUiInBrowser(exeDir, L"poe2", L"zh-rTW", bcfg, L"", sandbox); });
			std::string url;
			for (int i = 0; i < 120 && url.empty(); i++) {
				url = ReadFileA(urlFile);
				if (url.empty()) Sleep(500);
			}
			int port = 0;
			std::string prefix;
			if (url.rfind("http://127.0.0.1:", 0) == 0) {
				port = atoi(url.c_str() + 17);
				size_t slash = url.find('/', 17);
				if (slash != std::string::npos) prefix = url.substr(slash);
			}
			check("browser mode: the server listens on 127.0.0.1 under a /t/<token>/ URL", port > 0 && prefix.size() > 30, url);

			WSADATA wsa{};
			WSAStartup(MAKEWORD(2, 2), &wsa);
			auto connectLocal = [&]() -> SOCKET {
				SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				sockaddr_in a{};
				a.sin_family = AF_INET;
				a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				a.sin_port = htons((u_short)port);
				if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
				DWORD tmo = 15000;
				setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
				return s;
			};
			// one request, Connection: close -> status code and body
			auto http = [&](const std::string& method, const std::string& path, const std::string& host, const std::string& body, std::string& out) -> int {
				SOCKET s = connectLocal();
				if (s == INVALID_SOCKET) return -1;
				std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Length: " +
				                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
				send(s, req.data(), (int)req.size(), 0);
				std::string resp;
				char buf[16384];
				int n;
				while ((n = recv(s, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);
				closesocket(s);
				size_t sp = resp.find(' ');
				size_t he = resp.find("\r\n\r\n");
				out = he == std::string::npos ? std::string() : resp.substr(he + 4);
				return sp == std::string::npos ? 0 : atoi(resp.c_str() + sp + 1);
			};
			const std::string host = "127.0.0.1:" + std::to_string(port);
			std::string body;
			int page = port ? http("GET", prefix, host, "", body) : -1;
			check("browser mode: the page is served with the window.pobtools boot script injected",
			      page == 200 && body.find("window.pobtools") != std::string::npos && body.find("\"browser\":true") != std::string::npos,
			      std::to_string(page));
			int font = port ? http("GET", prefix + "~fonts/NotoSansTC-Regular.ttf", host, "", body) : -1;
			int badTok = port ? http("GET", "/t/00000000000000000000000000000000/", host, "", body) : -1;
			int climb = port ? http("GET", prefix + "~pob/../../pob-zh.ini", host, "", body) : -1;
			int badHost = port ? http("GET", prefix, "evil.example", "", body) : -1;
			check("browser mode: fonts are served; a wrong token, climbing out and a foreign Host header are refused",
			      font == 200 && badTok == 404 && climb == 403 && badHost == 421,
			      "font=" + std::to_string(font) + " token=" + std::to_string(badTok) + " climb=" + std::to_string(climb) + " host=" + std::to_string(badHost));

			// the event stream: the engine's hello, then a host.info answer
			SOCKET es = port ? connectLocal() : INVALID_SOCKET;
			std::string stream;
			bool gotHello = false, gotInfo = false;
			if (es != INVALID_SOCKET) {
				std::string req = "GET " + prefix + "~events HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
				send(es, req.data(), (int)req.size(), 0);
				char buf[16384];
				const ULONGLONG until = GetTickCount64() + 120000;
				bool asked = false;
				while (GetTickCount64() < until && !gotInfo) {
					int n = recv(es, buf, sizeof(buf), 0);
					if (n <= 0) break;
					stream.append(buf, n);
					if (!gotHello && stream.find("\"event\":\"hello\"") != std::string::npos) gotHello = true;
					if (gotHello && !asked) {
						asked = true;
						std::string ignored;
						http("POST", prefix + "~send", host, "{\"id\":77001,\"method\":\"host.info\",\"params\":{}}", ignored);
					}
					if (stream.find("\"id\":77001") != std::string::npos && stream.find("\"browser\":true", stream.find("\"id\":77001")) != std::string::npos) gotInfo = true;
				}
			}
			check("browser mode: the event stream carries the engine's hello and a POSTed host.info comes back on it",
			      gotHello && gotInfo, stream.substr(0, 300));
			// every message carries an id; a page that reconnects with Last-Event-ID
			// continues after it instead of getting the whole history again
			size_t lastId = std::string::npos;
			for (size_t at = stream.find("\nid: "); at != std::string::npos; at = stream.find("\nid: ", at + 1)) {
				lastId = (size_t)strtoull(stream.c_str() + at + 5, nullptr, 10);
			}
			if (lastId == std::string::npos && stream.find("\r\n\r\nid: ") != std::string::npos) lastId = 0;
			bool resumed = false;
			std::string again;
			SOCKET es2 = (port && lastId != std::string::npos) ? connectLocal() : INVALID_SOCKET;
			if (es2 != INVALID_SOCKET) {
				std::string req = "GET " + prefix + "~events HTTP/1.1\r\nHost: " + host + "\r\nLast-Event-ID: " + std::to_string(lastId) + "\r\n\r\n";
				send(es2, req.data(), (int)req.size(), 0);
				std::string ignored;
				http("POST", prefix + "~send", host, "{\"id\":77003,\"method\":\"host.info\",\"params\":{}}", ignored);
				char buf[16384];
				const ULONGLONG until = GetTickCount64() + 30000;
				while (GetTickCount64() < until && again.find("\"id\":77003") == std::string::npos) {
					int n = recv(es2, buf, sizeof(buf), 0);
					if (n <= 0) break;
					again.append(buf, n);
				}
				const size_t firstId = again.find("\nid: ");
				resumed = again.find("\"id\":77003") != std::string::npos && again.find("\"event\":\"hello\"") == std::string::npos &&
				          firstId != std::string::npos && strtoull(again.c_str() + firstId + 5, nullptr, 10) > lastId;
				closesocket(es2);
			}
			check("browser mode: events carry ids and a reconnect with Last-Event-ID resumes after it (no replayed hello)",
			      resumed, "last=" + std::to_string(lastId) + " " + again.substr(0, 200));

			// one per game: the launcher sees it, and launching again reuses it
			std::string runningUrl;
			const bool seen = ModernUiBrowserRunning(exeDir, L"poe2", &runningUrl) && runningUrl == url;
			const ULONGLONG t0 = GetTickCount64();
			const int second = ShowModernUiInBrowser(exeDir, L"poe2", L"zh-rTW", bcfg, L"", sandbox);
			const ULONGLONG took = GetTickCount64() - t0;
			check("browser mode: the running session is found by its URL file, and a second launch reuses it and returns at once",
			      seen && second == 0 && took < 5000 && ReadFileA(urlFile) == url && brc.load() == -1000,
			      "seen=" + std::to_string(seen) + " rc=" + std::to_string(second) + " ms=" + std::to_string(took));

			// ended from outside (the launcher's End): the open page is told first
			const bool stopAsked = ModernUiBrowserStop(exeDir, L"poe2");
			bool toldClosed = false;
			if (es != INVALID_SOCKET) {
				char buf[16384];
				const ULONGLONG until = GetTickCount64() + 15000;
				while (GetTickCount64() < until && !toldClosed) {
					int n = recv(es, buf, sizeof(buf), 0);
					if (n <= 0) break;
					stream.append(buf, n);
					toldClosed = stream.find("\"event\":\"host.closed\"") != std::string::npos;
				}
				closesocket(es);
			}
			bool stopped = false;
			for (int i = 0; i < 60 && !stopped; i++) {
				if (brc.load() != -1000) stopped = true; else Sleep(500);
			}
			check("browser mode: ModernUiBrowserStop ends the server (exit 0), the page gets host.closed, the URL file is removed",
			      stopAsked && toldClosed && stopped && brc.load() == 0 && GetFileAttributesW(urlFile.c_str()) == INVALID_FILE_ATTRIBUTES &&
			          !ModernUiBrowserRunning(exeDir, L"poe2"),
			      "stop=" + std::to_string(stopAsked) + " told=" + std::to_string(toldClosed) + " rc=" + std::to_string(brc.load()));

			// a URL file left by a killed session is not taken for a running one
			{
				HANDLE h = CreateFileW(urlFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
				const std::string stale = "http://127.0.0.1:1/t/00000000000000000000000000000000/";
				DWORD w = 0;
				if (h != INVALID_HANDLE_VALUE) { WriteFile(h, stale.data(), (DWORD)stale.size(), &w, nullptr); CloseHandle(h); }
			}
			check("browser mode: a stale URL file (nothing listening) is not running and is removed",
			      !ModernUiBrowserRunning(exeDir, L"poe2") && GetFileAttributesW(urlFile.c_str()) == INVALID_FILE_ATTRIBUTES, "");
			if (stopped) server.join(); else server.detach();
			WSACleanup();
			SetEnvironmentVariableW(L"POB_ZH_NO_BROWSER", nullptr);
		}

		Rmtree(sandbox);
	}

	line("PASS " + std::to_string(g_pass) + "   FAIL " + std::to_string(g_fail));
	line(g_fail == 0 ? "ALL PASS" : "FAILURES");
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", g_rep.c_str());
	WriteFileA(reportPath, g_rep);
	return g_fail;
}
