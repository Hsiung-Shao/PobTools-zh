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

#include "error_log.h"
#include "launcher_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <cmath>
#include <cstdio>
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
				if (MakeJunction(dst + L"\\" + n, src + L"\\" + n)) st.junctions++; else st.errors++;
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
	size_t a = manifest.find("<Version number=\"");
	if (a == std::string::npos) return {};
	a += strlen("<Version number=\"");
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
				bool ok = child.Call(method, p, r, 120000);
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
		check("sandbox TreeData is a junction (not a copy)", IsReparse(sandbox + L"\\TreeData"));

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
			json bd;
			bool okBd = firstBd > 0 && child.Call("sidebar_breakdown", json{{"rowIndex", firstBd}}, bd, 30000);
			check("sidebar_breakdown of the first breakdown row has sections",
			      okBd && bd.contains("sections") && !bd["sections"].empty(),
			      okBd ? bd.dump().substr(0, 300) : ("row=" + std::to_string(firstBd) + " " + bd.dump()));
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

		// --- POB's own update check, synchronously -----------------------------
		json upd;
		bool okUpd = child.Call("check_update_sync", json::object(), upd, 300000);
		check("check_update_sync answered", okUpd, upd.dump());
		if (okUpd) {
			check("update check on an untouched sandbox says \"none\"",
			      upd.value("mode", "") == "none",
			      "mode=" + upd.value("mode", "") + " error=" + upd.value("error", ""));
		}

		// --- graceful shutdown --------------------------------------------------
		child.Stop(10000);
		bool exited = child.WaitExit(0);
		check("child exits when the host closes stdin", exited && child.ExitCode() == 0,
		      "exit=" + std::to_string((long)child.ExitCode()));
		std::string stray = child.StrayOutput();
		if (!stray.empty()) line("INFO non-JSON output from child:\r\n" + stray.substr(0, 2000));

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
