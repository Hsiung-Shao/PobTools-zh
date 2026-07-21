#include "launcher_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

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

InstallInfo DetectInstalls(const std::wstring& exeDir)
{
	InstallInfo info;
	std::wstring poe1Dir = exeDir + L"PathOfBuildingCommunity";
	std::wstring poe2Dir = exeDir + L"PathOfBuildingCommunity-PoE2-Portable";
	std::wstring poe1 = poe1Dir + L"\\Launch.lua";
	std::wstring poe2 = poe2Dir + L"\\Launch.lua";
	if (file_exists(poe1)) {
		info.poe1Lua = poe1;
		info.poe1Version = read_pob_version(poe1Dir);
	}
	if (file_exists(poe2)) {
		info.poe2Lua = poe2;
		info.poe2Version = read_pob_version(poe2Dir);
	}
	return info;
}
