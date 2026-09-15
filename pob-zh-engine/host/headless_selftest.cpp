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
