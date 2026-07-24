#include "launcher_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <initializer_list>

// Config section name after the PobCharm -> PobTools rename. Old pob-zh.ini files
// store their keys under [PoeCharm]; we read that as a one-time migration fallback
// but always write the new section, so the file upgrades itself on first save.
static const wchar_t* kSection    = L"PobTools";
static const wchar_t* kSectionOld = L"PoeCharm";

const wchar_t* const kDefaultFontFile = L"NotoSansTC-Regular.ttf";

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a string key from [PobTools]; if it is absent, fall back to the legacy
// [PoeCharm] section (a sentinel default distinguishes "absent" from "empty").
static void read_ini_str(const std::wstring& iniPath, const wchar_t* key,
                         const wchar_t* fallbackDefault, wchar_t* buf, DWORD bufSize)
{
	static const wchar_t* kSentinel = L"\x01\x7f";
	GetPrivateProfileStringW(kSection, key, kSentinel, buf, bufSize, iniPath.c_str());
	if (wcscmp(buf, kSentinel) == 0)
		GetPrivateProfileStringW(kSectionOld, key, fallbackDefault, buf, bufSize, iniPath.c_str());
}

// Read an int key from [PobTools], falling back to legacy [PoeCharm].
static int read_ini_int(const std::wstring& iniPath, const wchar_t* key, int fallbackDefault)
{
	const int kSentinel = INT_MIN;
	int v = GetPrivateProfileIntW(kSection, key, kSentinel, iniPath.c_str());
	if (v == kSentinel) v = GetPrivateProfileIntW(kSectionOld, key, fallbackDefault, iniPath.c_str());
	return v;
}

LauncherConfig LoadLauncherConfig(const std::wstring& iniPath)
{
	LauncherConfig c;
	wchar_t buf[64];

	// Environment variables (long-lived user setup) take priority as defaults;
	// the UI's final choice overwrites them via set_env_both before launch.
	DWORD n = GetEnvironmentVariableW(L"POB_GAME", buf, 64);
	if (n > 0 && n < 64) {
		c.game = buf;
	} else {
		read_ini_str(iniPath, L"Game", L"poe1", buf, 64);
		c.game = buf;
	}

	n = GetEnvironmentVariableW(L"POB_LOCALE", buf, 64);
	if (n > 0 && n < 64) {
		c.locale = buf;
	} else {
		read_ini_str(iniPath, L"Locale", L"zh-rTW", buf, 64);
		c.locale = buf;
	}

	c.returnToLauncher = read_ini_int(iniPath, L"ReturnToLauncher", 0) != 0;

	wchar_t fbuf[128];
	read_ini_str(iniPath, L"Font", kDefaultFontFile, fbuf, 128);
	c.fontFile = fbuf;
	if (c.fontFile.empty()) c.fontFile = kDefaultFontFile;

	if (c.game != L"poe1" && c.game != L"poe2") c.game = L"poe1";
	if (c.locale != L"zh-rTW" && c.locale != L"en") c.locale = L"zh-rTW";
	return c;
}

void SaveLauncherConfig(const std::wstring& iniPath, const LauncherConfig& cfg)
{
	WritePrivateProfileStringW(kSection, L"Game", cfg.game.c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"Locale", cfg.locale.c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"ReturnToLauncher", cfg.returnToLauncher ? L"1" : L"0", iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"Font", cfg.fontFile.c_str(), iniPath.c_str());
}

std::vector<std::wstring> ListAvailableFonts(const std::wstring& exeDir)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((exeDir + L"Fonts\\*.ttf").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				out.push_back(fd.cFileName);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	return out;
}

std::wstring ResolveFontPath(const std::wstring& exeDir, const std::wstring& fontFile)
{
	const std::wstring dir = exeDir + L"Fonts\\";
	auto tryName = [&](const std::wstring& f) -> std::wstring {
		return (!f.empty() && file_exists(dir + f)) ? dir + f : std::wstring();
	};
	std::wstring p = tryName(fontFile);
	if (p.empty()) p = tryName(kDefaultFontFile);
	if (p.empty()) p = tryName(L"FZ_ZY.ttf");
	if (p.empty()) {
		auto all = ListAvailableFonts(exeDir);
		if (!all.empty()) p = dir + all[0];
	}
	if (p.empty()) p = dir + fontFile; // last resort; caller handles a failed read
	return p;
}

std::wstring ResolveConfiguredFontPath(const std::wstring& exeDir)
{
	wchar_t fbuf[128];
	read_ini_str(exeDir + L"pob-zh.ini", L"Font", kDefaultFontFile, fbuf, 128);
	std::wstring f = fbuf;
	if (f.empty()) f = kDefaultFontFile;
	return ResolveFontPath(exeDir, f);
}

// Best-effort POB version from <install dir>\manifest.xml. The attribute order
// differs between installs (platform can precede or follow number), so find the
// <Version tag first, then number="..." inside it.
static std::string read_pob_version(const std::wstring& installDir)
{
	HANDLE h = CreateFileW((installDir + L"\\manifest.xml").c_str(), GENERIC_READ, FILE_SHARE_READ,
	                       nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return std::string();
	std::string xml;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 22)) {
		xml.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, &xml[0], (DWORD)xml.size(), &read, nullptr) || read != xml.size()) xml.clear();
	}
	CloseHandle(h);
	if (xml.empty()) return std::string();

	size_t tag = xml.find("<Version");
	if (tag == std::string::npos) return std::string();
	size_t end = xml.find('>', tag);
	if (end == std::string::npos) return std::string();
	size_t attr = xml.find("number=\"", tag);
	if (attr == std::string::npos || attr > end) return std::string();
	attr += 8;
	size_t close = xml.find('"', attr);
	if (close == std::string::npos || close > end) return std::string();
	std::string v = xml.substr(attr, close - attr);
	if (v.empty() || v.size() >= 16) return std::string();
	for (char c : v)
		if (!(c >= '0' && c <= '9') && c != '.') return std::string();
	return v;
}

static bool dir_has_launch_lua(const std::wstring& dir)
{
	return file_exists(dir + L"\\Launch.lua");
}

// Folder-name heuristic: the PoE2 fork ships as *-PoE2-* and users keep "poe2"
// in renamed copies; everything else is treated as a PoE1 install.
static bool name_looks_poe2(const std::wstring& name)
{
	std::wstring low;
	low.reserve(name.size());
	for (wchar_t c : name) low.push_back((wchar_t)towlower(c));
	return low.find(L"poe2") != std::wstring::npos;
}

InstallInfo DetectInstalls(const std::wstring& exeDir)
{
	InstallInfo info;

	// 1) canonical names: existing installs keep resolving exactly as before.
	static const wchar_t* kPoe1Names[] = { L"PathOfBuildingCommunity", L"PathOfBuildingCommunity-Portable" };
	static const wchar_t* kPoe2Names[] = { L"PathOfBuildingCommunity-PoE2-Portable" };
	for (const wchar_t* n : kPoe1Names)
		if (info.poe1Dir.empty() && dir_has_launch_lua(exeDir + n)) info.poe1Dir = exeDir + n;
	for (const wchar_t* n : kPoe2Names)
		if (info.poe2Dir.empty() && dir_has_launch_lua(exeDir + n)) info.poe2Dir = exeDir + n;

	// 2) any other first-level subfolder holding Launch.lua; the folder name
	//    decides the game slot. Sorted so a multi-candidate pick is deterministic.
	if (info.poe1Dir.empty() || info.poe2Dir.empty()) {
		std::vector<std::wstring> subs;
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((exeDir + L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
				subs.push_back(fd.cFileName);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		std::sort(subs.begin(), subs.end());
		for (const std::wstring& name : subs) {
			std::wstring full = exeDir + name;
			if (full == info.poe1Dir || full == info.poe2Dir) continue;
			if (!dir_has_launch_lua(full)) continue;
			std::wstring& slot = name_looks_poe2(name) ? info.poe2Dir : info.poe1Dir;
			if (slot.empty()) slot = full;
		}
	}

	// 3) pob-zh.exe dropped inside a POB folder (or POB unpacked beside the exe):
	//    Launch.lua right next to the exe, classified by the folder's own name.
	if (file_exists(exeDir + L"Launch.lua")) {
		std::wstring trimmed = exeDir;
		while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
		size_t slash = trimmed.find_last_of(L"\\/");
		std::wstring leaf = (slash == std::wstring::npos) ? trimmed : trimmed.substr(slash + 1);
		std::wstring& slot = name_looks_poe2(leaf) ? info.poe2Dir : info.poe1Dir;
		if (slot.empty() && !trimmed.empty()) slot = trimmed;
	}

	if (!info.poe1Dir.empty()) {
		info.poe1Lua = info.poe1Dir + L"\\Launch.lua";
		info.poe1Version = read_pob_version(info.poe1Dir);
	}
	if (!info.poe2Dir.empty()) {
		info.poe2Lua = info.poe2Dir + L"\\Launch.lua";
		info.poe2Version = read_pob_version(info.poe2Dir);
	}
	return info;
}

std::wstring FindPoe1Dir(const std::wstring& exeDir)
{
	std::wstring d = DetectInstalls(exeDir).poe1Dir;
	if (!d.empty()) d += L'\\';
	return d;
}

// ---- --detect-selftest --------------------------------------------------------

static bool st_mkdir(const std::wstring& p)
{
	return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool st_touch(const std::wstring& p)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	CloseHandle(h);
	return true;
}

// shallow recursive delete; the selftest sandboxes are at most two levels deep
static void st_rmtree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
			std::wstring p = dir + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) st_rmtree(p);
			else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

int RunDetectInstallsSelfTest(const std::wstring& exeDir)
{
	wchar_t tmp[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, tmp);
	if (n == 0 || n >= MAX_PATH) return 1;
	std::wstring root = std::wstring(tmp) + L"pobtools_detect_st";
	st_rmtree(root);
	if (!st_mkdir(root)) return 1;

	std::string report;
	int failures = 0;
	auto narrow = [](const std::wstring& w) {
		std::string s;
		for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?');
		return s;
	};
	auto check = [&](const char* name, bool ok, const std::string& detail) {
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};
	// sandbox exe-root: each listed subfolder gets a Launch.lua inside
	int caseNo = 0;
	auto makeRoot = [&](std::initializer_list<const wchar_t*> pobDirs, bool rootLua,
	                    const wchar_t* rootName = nullptr) {
		std::wstring r = rootName ? root + L"\\" + rootName
		                          : root + L"\\case" + std::to_wstring(++caseNo);
		st_mkdir(r);
		for (const wchar_t* d : pobDirs) {
			st_mkdir(r + L"\\" + d);
			st_touch(r + L"\\" + d + L"\\Launch.lua");
		}
		if (rootLua) st_touch(r + L"\\Launch.lua");
		return r + L"\\"; // production exeDir always carries a trailing backslash
	};

	{ // T1: both canonical folders resolve exactly as before
		std::wstring r = makeRoot({ L"PathOfBuildingCommunity", L"PathOfBuildingCommunity-PoE2-Portable" }, false);
		InstallInfo i = DetectInstalls(r);
		check("T1 canonical both",
		      i.poe1Dir == r + L"PathOfBuildingCommunity" &&
		      i.poe2Dir == r + L"PathOfBuildingCommunity-PoE2-Portable" &&
		      i.poe1Lua == i.poe1Dir + L"\\Launch.lua" &&
		      i.poe2Lua == i.poe2Dir + L"\\Launch.lua",
		      narrow(i.poe1Dir + L" | " + i.poe2Dir));
	}
	{ // T2: the official portable folder name counts as PoE1
		std::wstring r = makeRoot({ L"PathOfBuildingCommunity-Portable" }, false);
		InstallInfo i = DetectInstalls(r);
		check("T2 portable name",
		      i.poe1Dir == r + L"PathOfBuildingCommunity-Portable" && i.poe2Dir.empty(),
		      narrow(i.poe1Dir));
	}
	{ // T3: arbitrary names, classified by the "poe2" substring
		std::wstring r = makeRoot({ L"MyPob", L"PathOfBuilding-PoE2" }, false);
		InstallInfo i = DetectInstalls(r);
		check("T3 arbitrary names",
		      i.poe1Dir == r + L"MyPob" && i.poe2Dir == r + L"PathOfBuilding-PoE2",
		      narrow(i.poe1Dir + L" | " + i.poe2Dir));
	}
	{ // T4: Launch.lua next to the exe itself; slot from the folder's own name
		std::wstring r1 = makeRoot({}, true, L"case4_plain");
		InstallInfo a = DetectInstalls(r1);
		std::wstring r2 = makeRoot({}, true, L"case4_poe2_copy");
		InstallInfo b = DetectInstalls(r2);
		check("T4 root-level lua",
		      a.poe1Dir + L"\\" == r1 && a.poe2Dir.empty() &&
		      b.poe2Dir + L"\\" == r2 && b.poe1Dir.empty(),
		      narrow(a.poe1Dir + L" | " + b.poe2Dir));
	}
	{ // T5: canonical wins over a renamed sibling that sorts first
		std::wstring r = makeRoot({ L"AAA_Renamed", L"PathOfBuildingCommunity" }, false);
		InstallInfo i = DetectInstalls(r);
		check("T5 canonical priority", i.poe1Dir == r + L"PathOfBuildingCommunity", narrow(i.poe1Dir));
	}
	{ // T6: subfolders without Launch.lua are ignored
		std::wstring r = makeRoot({}, false, L"case6");
		st_mkdir(root + L"\\case6\\engine");
		st_mkdir(root + L"\\case6\\Data");
		InstallInfo i = DetectInstalls(r);
		check("T6 none", i.poe1Dir.empty() && i.poe2Dir.empty() && i.poe1Lua.empty(), "");
	}
	{ // T7: FindPoe1Dir contract (trailing backslash / empty)
		std::wstring r = makeRoot({ L"MyPob" }, false);
		std::wstring d = FindPoe1Dir(r);
		std::wstring none = FindPoe1Dir(root + L"\\case6\\");
		check("T7 FindPoe1Dir", d == r + L"MyPob\\" && none.empty(), narrow(d));
	}

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\detect_selftest.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	st_rmtree(root);
	return failures ? 2 : 0;
}
