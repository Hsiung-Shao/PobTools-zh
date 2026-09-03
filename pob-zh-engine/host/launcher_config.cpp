#include "launcher_config.h"
#include "error_log.h"

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

// Index-aligned with DictSlot. One table so the folder name, the ini key and the
// UI order cannot drift apart.
static const wchar_t* const kDictSlotFolders[kDictSlotCount] = { L"poe1", L"poe2", L"launcher" };
static const wchar_t* const kDataDirKeys[kDictSlotCount] = { L"DataDirPoe1", L"DataDirPoe2",
                                                             L"DataDirLauncher" };

const wchar_t* DictSlotFolder(DictSlot slot)
{
	const int i = (int)slot;
	return (i >= 0 && i < kDictSlotCount) ? kDictSlotFolders[i] : kDictSlotFolders[0];
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dir_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- path helpers ----------------------------------------------------------

// One trailing backslash, no leading/trailing spaces. The trailing separator is
// load-bearing for PathIsInside: without it "…\Data" prefix-matches "…\Data2".
static std::wstring with_trailing_sep(std::wstring p)
{
	while (!p.empty() && (p.front() == L' ' || p.front() == L'"')) p.erase(p.begin());
	while (!p.empty() && (p.back() == L' ' || p.back() == L'"')) p.pop_back();
	for (wchar_t& c : p) if (c == L'/') c = L'\\';
	while (p.size() > 1 && p.back() == L'\\' && p[p.size() - 2] == L'\\') p.pop_back();
	if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
	return p;
}

static std::wstring lower(std::wstring s)
{
	for (wchar_t& c : s) c = (wchar_t)towlower(c);
	return s;
}

bool PathIsInside(const std::wstring& parentDir, const std::wstring& path)
{
	if (parentDir.empty() || path.empty()) return false;
	const std::wstring p = lower(with_trailing_sep(parentDir));
	const std::wstring c = lower(with_trailing_sep(path));
	return c.size() >= p.size() && c.compare(0, p.size(), p) == 0;
}

// ---- narrow/widen (UTF-8) --------------------------------------------------

static std::string narrow_utf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::wstring widen_utf8(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
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

// Read a key whose value is a PATH. The fixed 64/128-wchar buffers the other keys
// use TRUNCATE anything longer, and GetPrivateProfileStringW gives no way to tell a
// truncated value from a short one -- the symptom would be "the folder I chose is
// being ignored", but only for long paths, which is the hardest kind to reproduce.
static std::wstring read_ini_path(const std::wstring& iniPath, const wchar_t* key)
{
	std::vector<wchar_t> buf(2048, L'\0');
	read_ini_str(iniPath, key, L"", buf.data(), (DWORD)buf.size());
	return std::wstring(buf.data());
}

// pob-zh.ini carries no BOM, so WritePrivateProfileStringW encodes with the system
// ANSI codepage. That works here (cp950 covers Traditional Chinese) right up until
// the machine is switched to the UTF-8 ACP ("Beta: Use Unicode UTF-8 for worldwide
// language support"), after which a path written under one codepage reads back as
// mojibake under the other -- silently. A non-ASCII path therefore also gets a
// pure-ASCII hex copy that the reader prefers; ASCII paths skip it so the common
// case stays hand-editable.
static bool is_ascii(const std::wstring& w)
{
	for (wchar_t c : w) if (c >= 0x80) return false;
	return true;
}

static std::wstring hex_of_utf8(const std::wstring& w)
{
	static const wchar_t* kHex = L"0123456789abcdef";
	std::string bytes = narrow_utf8(w);
	std::wstring out;
	out.reserve(bytes.size() * 2);
	for (unsigned char b : bytes) { out.push_back(kHex[b >> 4]); out.push_back(kHex[b & 0xF]); }
	return out;
}

static std::wstring utf8_of_hex(const std::wstring& hex)
{
	auto nib = [](wchar_t c) -> int {
		if (c >= L'0' && c <= L'9') return c - L'0';
		if (c >= L'a' && c <= L'f') return c - L'a' + 10;
		if (c >= L'A' && c <= L'F') return c - L'A' + 10;
		return -1;
	};
	if (hex.size() % 2) return std::wstring();
	std::string bytes;
	bytes.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		int hi = nib(hex[i]), lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return std::wstring(); // garbage: fall back to the raw key
		bytes.push_back((char)((hi << 4) | lo));
	}
	return widen_utf8(bytes);
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

	// ExitMode replaced the ReturnToLauncher boolean. Absent key = an ini written
	// by an older build, so migrate from it; anything out of range (hand-edited,
	// or written by a future version) falls back to the historical behaviour
	// rather than to an undefined value.
	{
		int em = read_ini_int(iniPath, L"ExitMode", INT_MIN);
		if (em == INT_MIN)
			em = read_ini_int(iniPath, L"ReturnToLauncher", 0) != 0 ? 1 : 0;
		if (em < 0 || em > 2) em = 0;
		c.exitMode = (LaunchExitMode)em;

		const int st = read_ini_int(iniPath, L"StartupTab", 0);
		c.startupTab = (st == 1) ? StartupTab::Versions : StartupTab::Home;

		// Anything that is not exactly 1 means Separate, so a hand-edited or
		// future value can never leave someone stuck in the new path.
		c.windowMode = (read_ini_int(iniPath, L"WindowMode", 0) == 1)
		             ? WindowMode::Tabbed : WindowMode::Separate;

		// Remembered window sizes. A pair is only meaningful whole: a width with
		// no usable height (one key hand-deleted, one value garbage) is "default",
		// not "default height with a remembered width".
		auto readPair = [&](const wchar_t* wKey, const wchar_t* hKey, int& w, int& h) {
			w = ClampWindowDim(read_ini_int(iniPath, wKey, 0));
			h = ClampWindowDim(read_ini_int(iniPath, hKey, 0));
			if (w == 0 || h == 0) w = h = 0;
		};
		readPair(L"WindowW", L"WindowH", c.winW, c.winH);
		readPair(L"TabbedWindowW", L"TabbedWindowH", c.tabWinW, c.tabWinH);
	}

	wchar_t fbuf[128];
	read_ini_str(iniPath, L"Font", kDefaultFontFile, fbuf, 128);
	c.fontFile = fbuf;
	if (c.fontFile.empty()) c.fontFile = kDefaultFontFile;
	c.fontApplyAll = read_ini_int(iniPath, L"FontApplyAll", 1) != 0;
	c.fontSize = ClampLauncherFontSize(read_ini_int(iniPath, L"FontSize", kLauncherFontSizeDefault));

	// Hex copy first (see hex_of_utf8): it is the codepage-proof spelling. A
	// malformed one decodes to empty and we fall through to the raw key rather
	// than losing the setting.
	for (int i = 0; i < kDictSlotCount; i++) {
		const std::wstring key = kDataDirKeys[i];
		std::wstring decoded = utf8_of_hex(read_ini_path(iniPath, (key + L"Hex").c_str()));
		c.dataDir[i] = decoded.empty() ? read_ini_path(iniPath, key.c_str()) : decoded;
	}

	c.updateTranslations = read_ini_int(iniPath, L"UpdateTranslations", 1) != 0;

	c.proxy = read_ini_path(iniPath, L"Proxy");

	if (c.game != L"poe1" && c.game != L"poe2") c.game = L"poe1";
	// Deliberately NOT clamped to a list of known language codes. This function
	// only sees an ini path, so it cannot know which language folders exist; the
	// old two-string clamp made every added folder unreachable. Validation belongs
	// where the disk is visible -- see ListInstalledLocales / PickLocaleIndex,
	// the same split already used for the dictionary folders.
	if (c.locale.empty()) c.locale = L"zh-rTW";
	return c;
}

void SaveLauncherConfig(const std::wstring& iniPath, const LauncherConfig& cfg)
{
	// Only the first write is checked. Every line below goes to the same file
	// through the same API, so they fail together (read-only install, folder gone,
	// a security product holding the handle) -- checking one is enough to notice,
	// and checking all twenty would bury the signal in repetition.
	if (!WritePrivateProfileStringW(kSection, L"Game", cfg.game.c_str(), iniPath.c_str())) {
		PobLog::Error("config", "pob-zh.ini could not be written, GetLastError=" +
		                            std::to_string((unsigned long)GetLastError()) +
		                            " (settings will not survive a restart)");
	}
	WritePrivateProfileStringW(kSection, L"Locale", cfg.locale.c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"ExitMode",
		std::to_wstring((int)cfg.exitMode).c_str(), iniPath.c_str());
	// Mirrored for older builds: someone who downgrades keeps their "return to
	// launcher" choice instead of silently losing it.
	WritePrivateProfileStringW(kSection, L"ReturnToLauncher",
		cfg.exitMode == LaunchExitMode::ReturnAfterExit ? L"1" : L"0", iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"StartupTab",
		std::to_wstring((int)cfg.startupTab).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"WindowMode",
		std::to_wstring((int)cfg.windowMode).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"WindowW",
		std::to_wstring(cfg.winW).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"WindowH",
		std::to_wstring(cfg.winH).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"TabbedWindowW",
		std::to_wstring(cfg.tabWinW).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"TabbedWindowH",
		std::to_wstring(cfg.tabWinH).c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"Font", cfg.fontFile.c_str(), iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"FontApplyAll",
		cfg.fontApplyAll ? L"1" : L"0", iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"FontSize",
		std::to_wstring(cfg.fontSize).c_str(), iniPath.c_str());

	for (int i = 0; i < kDictSlotCount; i++) {
		const std::wstring key = kDataDirKeys[i];
		const std::wstring hexKey = key + L"Hex";
		WritePrivateProfileStringW(kSection, key.c_str(), cfg.dataDir[i].c_str(), iniPath.c_str());
		if (cfg.dataDir[i].empty() || is_ascii(cfg.dataDir[i]))
			WritePrivateProfileStringW(kSection, hexKey.c_str(), nullptr, iniPath.c_str()); // nullptr deletes
		else
			WritePrivateProfileStringW(kSection, hexKey.c_str(), hex_of_utf8(cfg.dataDir[i]).c_str(),
			                           iniPath.c_str());
	}
	// The single shared DataDir shipped in no release; drop it rather than leave a
	// dead key that looks like it still does something.
	WritePrivateProfileStringW(kSection, L"DataDir", nullptr, iniPath.c_str());
	WritePrivateProfileStringW(kSection, L"DataDirHex", nullptr, iniPath.c_str());

	WritePrivateProfileStringW(kSection, L"UpdateTranslations",
		cfg.updateTranslations ? L"1" : L"0", iniPath.c_str());

	WritePrivateProfileStringW(kSection, L"Proxy", cfg.proxy.c_str(), iniPath.c_str());
}

// ---- external translation-data root ----------------------------------------

// meta.json's load_order, read without pulling in the JSON library: this file is
// linked into every entry point and the array is a flat list of quoted names.
static std::vector<std::string> load_order_of(const std::wstring& metaPath)
{
	std::vector<std::string> out;
	HANDLE h = CreateFileW(metaPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return out;
	std::string text;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
		text.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, &text[0], (DWORD)text.size(), &read, nullptr) || read != text.size())
			text.clear();
	}
	CloseHandle(h);

	size_t k = text.find("\"load_order\"");
	if (k == std::string::npos) return out;
	size_t open = text.find('[', k), close = text.find(']', k);
	if (open == std::string::npos || close == std::string::npos || close < open) return out;
	for (size_t i = open + 1; i < close;) {
		size_t q = text.find('"', i);
		if (q == std::string::npos || q > close) break;
		size_t e = text.find('"', q + 1);
		if (e == std::string::npos || e > close) break;
		out.push_back(text.substr(q + 1, e - q - 1));
		i = e + 1;
	}
	return out;
}

static std::vector<std::wstring> subdirs_of(const std::wstring& dir)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.push_back(n);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

static int count_json(const std::wstring& dir)
{
	int n = 0;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

std::wstring BuiltinDictDir(const std::wstring& exeDir, DictSlot slot)
{
	return exeDir + L"Data\\" + DictSlotFolder(slot) + L"\\";
}

// Every <locale>\meta.json directly under `dir`, with its *.json count.
static std::vector<std::pair<std::string, int>> locales_in(const std::wstring& dir)
{
	std::vector<std::pair<std::string, int>> out;
	for (const std::wstring& loc : subdirs_of(dir)) {
		const std::wstring leaf = dir + loc + L"\\";
		if (file_exists(leaf + L"meta.json"))
			out.push_back({ narrow_utf8(loc), count_json(leaf) });
	}
	return out;
}

DictDirInfo ResolveDictDir(const std::wstring& exeDir, DictSlot slot, const std::wstring& configured)
{
	const std::wstring builtin = BuiltinDictDir(exeDir, slot);

	DictDirInfo info;
	info.configured = configured;
	info.root = builtin;
	if (configured.empty()) {
		info.status = DataDirStatus::Builtin;
		return info;
	}

	const std::wstring path = with_trailing_sep(configured);
	info.insideInstall = PathIsInside(exeDir, path);
	if (!dir_exists(path)) {
		info.status = DataDirStatus::Missing;
		return info;
	}

	info.found = locales_in(path);

	if (info.found.empty()) {
		// Both off-by-one-level mistakes are likely and look identical from a
		// generic "nothing here": one level too deep (picked the zh-rTW folder) and
		// one level too shallow (picked the Data folder). Name which, and hand back
		// the corrected path so the fix is one click.
		if (file_exists(path + L"meta.json")) {
			info.status = DataDirStatus::WrongShape;
			std::wstring up = path;
			if (up.size() > 1) up.pop_back();                       // drop trailing '\'
			const size_t cut = up.find_last_of(L'\\');
			if (cut != std::wstring::npos) info.suggestion = up.substr(0, cut + 1);
		} else if (!locales_in(path + DictSlotFolder(slot) + L"\\").empty()) {
			info.status = DataDirStatus::TooShallow;
			info.suggestion = path + DictSlotFolder(slot) + L"\\";
		} else {
			info.status = DataDirStatus::NoDictionaries;
		}
		return info;
	}

	// A dictionary added by a later release is not in an older external copy's
	// load_order, and the engine loads only what load_order names -- so it goes
	// missing with no error anywhere.
	for (const auto& f : info.found) {
		const std::wstring loc = widen_utf8(f.first);
		std::vector<std::string> mine = load_order_of(path + loc + L"\\meta.json");
		for (const std::string& name : load_order_of(builtin + loc + L"\\meta.json")) {
			bool have = false;
			for (const std::string& m : mine) if (m == name) { have = true; break; }
			if (!have) info.staleLoadOrder.push_back(f.first + ": " + name);
		}
	}

	info.status = DataDirStatus::External;
	info.root = path;
	return info;
}

bool DictionariesPresentAt(const std::wstring& dest)
{
	return !locales_in(with_trailing_sep(dest)).empty();
}

// ---- installed interface languages ------------------------------------------

// meta.json's optional "display_name". Same hand-rolled scan as load_order_of:
// this file is linked into every entry point and pulling in the JSON library for
// one string is not worth it. Empty when absent or unreadable.
static std::string display_name_of(const std::wstring& metaPath)
{
	HANDLE h = CreateFileW(metaPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return std::string();
	std::string text;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
		text.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(h, &text[0], (DWORD)text.size(), &read, nullptr) || read != text.size())
			text.clear();
	}
	CloseHandle(h);

	const size_t k = text.find("\"display_name\"");
	if (k == std::string::npos) return std::string();
	const size_t colon = text.find(':', k + 14);
	if (colon == std::string::npos) return std::string();
	const size_t q = text.find('"', colon);
	if (q == std::string::npos) return std::string();
	const size_t e = text.find('"', q + 1);
	if (e == std::string::npos) return std::string();
	return text.substr(q + 1, e - q - 1);
}

std::vector<LocaleInfo> ListInstalledLocales(const std::wstring& exeDir, const LauncherConfig& cfg)
{
	std::vector<LocaleInfo> out;

	// English first, always. It has no folder: the engine shows the original text
	// whenever it finds no dictionary, so scanning can never discover it.
	LocaleInfo en;
	en.id = "en";
	en.displayName = "English";
	out.push_back(en);

	for (int s = 0; s < kDictSlotCount; s++) {
		// The slot's ACTUAL folder, external one included -- someone who moved a
		// whole dictionary set out of the install must still see its languages.
		const std::wstring root = ResolveDictDir(exeDir, (DictSlot)s, cfg.dataDir[s]).root;
		for (const std::wstring& loc : subdirs_of(root)) {
			const std::wstring meta = root + loc + L"\\meta.json";
			if (!file_exists(meta)) continue;
			const std::string id = narrow_utf8(loc);
			if (id == "en") continue; // already there, and it needs no dictionary

			auto it = out.begin();
			for (; it != out.end(); ++it) if (it->id == id) break;
			if (it == out.end()) {
				LocaleInfo li;
				li.id = id;
				li.displayName = display_name_of(meta);
				if (li.displayName.empty()) li.displayName = id;
				out.push_back(li);
				it = out.end() - 1;
			} else if (it->displayName == it->id) {
				// A later slot may carry the display name the first one omitted.
				std::string dn = display_name_of(meta);
				if (!dn.empty()) it->displayName = dn;
			}
			it->slot[s] = true;
		}
	}

	// en stays pinned at index 0; sort the rest by id so the order does not depend
	// on which slot happened to be scanned first.
	std::sort(out.begin() + 1, out.end(),
	          [](const LocaleInfo& a, const LocaleInfo& b) { return a.id < b.id; });
	return out;
}

int PickLocaleIndex(const std::vector<LocaleInfo>& locales, const std::wstring& want)
{
	if (locales.empty()) return 0;
	const std::string id = narrow_utf8(want);
	for (size_t i = 0; i < locales.size(); i++) if (locales[i].id == id) return (int)i;
	for (size_t i = 0; i < locales.size(); i++) if (locales[i].id == "zh-rTW") return (int)i;
	return 0; // en
}

// Delete a folder and everything under it. Properly recursive: the selftest's
// sandbox is four levels deep (box\exe\Data\poe1\zh-rTW) and a hand-unrolled
// three-level version silently left the whole tree behind -- RemoveDirectoryW
// fails on a non-empty folder and reports nothing.
static void remove_tree(const std::wstring& dir)
{
	const std::wstring d = with_trailing_sep(dir);
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(d + n);
			else DeleteFileW((d + n).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(d.c_str());
}

static int copy_tree(const std::wstring& src, const std::wstring& dst, std::string* err)
{
	if (!dir_exists(dst) && !CreateDirectoryW(dst.c_str(), nullptr) &&
	    GetLastError() != ERROR_ALREADY_EXISTS) {
		if (err) *err = u8"無法建立資料夾：" + narrow_utf8(dst);
		return -1;
	}
	int n = 0;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((src + L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			int sub = copy_tree(src + name + L"\\", dst + name + L"\\", err);
			if (sub < 0) { FindClose(h); return -1; }
			n += sub;
		} else if (CopyFileW((src + name).c_str(), (dst + name).c_str(), FALSE)) {
			n++;
		} else {
			if (err) *err = u8"無法複製：" + narrow_utf8(name);
			FindClose(h);
			return -1;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

int CopyBuiltinDictionary(const std::wstring& exeDir, DictSlot slot,
                          const std::wstring& dest, std::string* err)
{
	const std::wstring src = BuiltinDictDir(exeDir, slot);
	const std::wstring dst = with_trailing_sep(dest);
	if (dst.empty()) {
		if (err) *err = u8"沒有選擇資料夾";
		return -1;
	}
	if (!dir_exists(src)) {
		if (err) *err = u8"內建資料夾裡沒有這一組字典";
		return -1;
	}
	int n = copy_tree(src, dst, err);
	if (n == 0 && err) *err = u8"內建資料夾裡沒有可複製的字典";
	return n <= 0 ? -1 : n;
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

std::vector<std::wstring> FallbackFontPaths(const std::wstring& exeDir, const std::wstring& fontFile)
{
	const std::wstring primary = ResolveFontPath(exeDir, fontFile);
	std::vector<std::wstring> out;
	for (const std::wstring& f : ListAvailableFonts(exeDir)) {
		const std::wstring p = exeDir + L"Fonts\\" + f;
		if (_wcsicmp(p.c_str(), primary.c_str()) != 0) out.push_back(p);
	}
	return out;
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
	// Release builds are "2.57.1"; the beta branch stamps "2.67.2-ed354c2f8"
	// (16 chars, dash, hex letters). The old digits-and-dots-under-16 guard
	// rejected exactly that and the launcher row showed no version at all for
	// a beta install. Cap 25 (user-set): longer than any real stamp, short
	// enough that markup or a corrupt attribute still reads as garbage.
	if (v.empty() || v.size() > 25) return std::string();
	for (char c : v) {
		const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		                (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
		if (!ok) return std::string();
	}
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

// ---- launcher config selftest ----------------------------------------------
//
// Everything worth testing about the exit-mode setting is in LoadLauncherConfig,
// which is a pure function of an ini file — so it can be exercised head-first
// with synthetic files and no window. That is a large part of why exit mode is
// an enum: two booleans would need tests for an illegal both-set state that the
// enum simply cannot represent.
int RunLauncherConfigSelfTest(const std::wstring& exeDir)
{
	wchar_t tmp[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, tmp);
	if (n == 0 || n >= MAX_PATH) return 1;
	const std::wstring ini = std::wstring(tmp) + L"pobtools_cfg_st.ini";

	std::string report;
	int failures = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail = "") {
		report += std::string(ok ? "PASS " : "FAIL ") + name +
		          (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};
	// The env vars would override Game/Locale and make these cases lie.
	SetEnvironmentVariableW(L"POB_GAME", nullptr);
	SetEnvironmentVariableW(L"POB_LOCALE", nullptr);

	auto write = [&](const wchar_t* section, std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> kv) {
		DeleteFileW(ini.c_str());
		for (const auto& p : kv)
			WritePrivateProfileStringW(section, p.first, p.second, ini.c_str());
	};
	auto mode = [&]() { return (int)LoadLauncherConfig(ini).exitMode; };
	auto tab  = [&]() { return (int)LoadLauncherConfig(ini).startupTab; };

	write(L"PobTools", { { L"ReturnToLauncher", L"1" } });
	check("T1 old ReturnToLauncher=1 migrates to ReturnAfterExit", mode() == 1, std::to_string(mode()));

	write(L"PobTools", { { L"ReturnToLauncher", L"0" } });
	check("T2 old ReturnToLauncher=0 migrates to CloseLauncher", mode() == 0, std::to_string(mode()));

	write(L"PobTools", { { L"ExitMode", L"2" }, { L"ReturnToLauncher", L"1" } });
	check("T3 ExitMode wins over the legacy key", mode() == 2, std::to_string(mode()));

	write(L"PobTools", { { L"ExitMode", L"7" } });
	check("T4 out-of-range ExitMode clamps to CloseLauncher", mode() == 0, std::to_string(mode()));

	write(L"PobTools", { { L"ExitMode", L"abc" } });
	check("T5 non-numeric ExitMode clamps to CloseLauncher", mode() == 0, std::to_string(mode()));

	write(L"PobTools", { { L"Game", L"poe1" } });
	check("T6 no exit-mode keys at all defaults to CloseLauncher", mode() == 0, std::to_string(mode()));

	write(L"PoeCharm", { { L"ReturnToLauncher", L"1" } });
	check("T7 legacy [PoeCharm] section still migrates", mode() == 1, std::to_string(mode()));

	write(L"PobTools", { { L"StartupTab", L"1" } });
	check("T8 StartupTab=1 selects the version tab", tab() == 1, std::to_string(tab()));
	write(L"PobTools", { { L"StartupTab", L"5" } });
	check("T9 out-of-range StartupTab clamps to Home", tab() == 0, std::to_string(tab()));

	// Window mode. The default matters more than the parsing here: the tabbed
	// path is new, so an ini that says nothing must land on the old behaviour.
	auto wm = [&]() { return (int)LoadLauncherConfig(ini).windowMode; };
	write(L"PobTools", { { L"Game", L"poe1" } });
	check("T8b no WindowMode key defaults to Separate", wm() == 0, std::to_string(wm()));
	write(L"PobTools", { { L"WindowMode", L"1" } });
	check("T8c WindowMode=1 selects Tabbed", wm() == 1, std::to_string(wm()));
	write(L"PobTools", { { L"WindowMode", L"7" } });
	check("T8d an unknown WindowMode falls back to Separate", wm() == 0, std::to_string(wm()));

	// round trip, including the mirrored legacy key
	for (int m = 0; m <= 2; m++) {
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.exitMode = (LaunchExitMode)m;
		c.startupTab = (m == 1) ? StartupTab::Versions : StartupTab::Home;
		c.windowMode = (m == 2) ? WindowMode::Tabbed : WindowMode::Separate;
		c.fontFile = kDefaultFontFile;
		SaveLauncherConfig(ini, c);
		LauncherConfig back = LoadLauncherConfig(ini);
		const int legacy = GetPrivateProfileIntW(kSection, L"ReturnToLauncher", -1, ini.c_str());
		check(("T10 round trip exit mode " + std::to_string(m)).c_str(),
		      (int)back.exitMode == m && (int)back.startupTab == (int)c.startupTab &&
		      (int)back.windowMode == (int)c.windowMode &&
		      legacy == (m == 1 ? 1 : 0),
		      "legacy=" + std::to_string(legacy));
	}

	// ---- external data root ------------------------------------------------

	// T11 -- the prefix traps. "Data2" sharing a prefix with "Data" is the one that
	// looks right until it silently marks an unrelated folder as inside the install.
	check("T11a exact folder counts as inside",      PathIsInside(L"D:\\app\\", L"D:\\app"));
	check("T11b case-insensitive",                   PathIsInside(L"D:\\App\\Data", L"d:\\app\\data\\poe1"));
	check("T11c trailing backslash irrelevant",      PathIsInside(L"D:\\app\\Data\\", L"D:\\app\\Data"));
	check("T11d sibling with a shared prefix is NOT inside", !PathIsInside(L"D:\\app\\Data", L"D:\\app\\Data2"));
	check("T11e unrelated path is not inside",       !PathIsInside(L"D:\\app", L"E:\\elsewhere"));
	check("T11f empty parent is never inside",       !PathIsInside(L"", L"D:\\app"));

	// T12 -- a path longer than the 128-wchar buffers the other keys use. The old
	// reader would truncate it and the setting would look like it was ignored.
	{
		std::wstring longPath = L"D:\\";
		while (longPath.size() < 400) longPath += L"translation_working_copy\\";
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.dataDir[0] = longPath;
		SaveLauncherConfig(ini, c);
		LauncherConfig back = LoadLauncherConfig(ini);
		check("T12 long DataDir round-trips without truncation", back.dataDir[0] == longPath,
		      "len " + std::to_string(back.dataDir[0].size()) + " vs " + std::to_string(longPath.size()));
	}

	// T13/T14 -- the hex copy exists for the codepage switch; it must appear only
	// when it is needed, or the ini stops being hand-editable for everyone else.
	{
		const std::wstring zh = L"D:\\翻譯\\工作副本\\";
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.dataDir[0] = zh;
		SaveLauncherConfig(ini, c);
		std::wstring hex = read_ini_path(ini, L"DataDirPoe1Hex");
		LauncherConfig back = LoadLauncherConfig(ini);
		check("T13 non-ASCII DataDir round-trips and writes a hex copy",
		      back.dataDir[0] == zh && !hex.empty(), narrow_utf8(hex));

		DeleteFileW(ini.c_str());
		c.dataDir[0] = L"D:\\plain\\ascii\\";
		SaveLauncherConfig(ini, c);
		check("T14 ASCII DataDir writes no hex key",
		      read_ini_path(ini, L"DataDirPoe1Hex").empty() &&
		          LoadLauncherConfig(ini).dataDir[0] == L"D:\\plain\\ascii\\");
	}

	// T15 -- when the two spellings disagree (an ANSI-mangled raw value is exactly
	// how that happens), the codepage-proof one has to win.
	write(L"PobTools", { { L"DataDirPoe1", L"D:\\mojibake" },
	                     { L"DataDirPoe1Hex", hex_of_utf8(L"D:\\good\\").c_str() } });
	check("T15 hex copy wins over a disagreeing raw value",
	      LoadLauncherConfig(ini).dataDir[0] == L"D:\\good\\");

	// T16 -- but a corrupt hex value must not throw the setting away.
	write(L"PobTools", { { L"DataDirPoe1", L"D:\\raw\\" }, { L"DataDirPoe1Hex", L"zznothex" } });
	check("T16 unparseable hex falls back to the raw value",
	      LoadLauncherConfig(ini).dataDir[0] == L"D:\\raw\\");

	// T16b -- the single shared DataDir from the first cut of this feature is
	// dropped on save. It never shipped, so there is nothing to migrate; leaving
	// it behind would just be a key that looks like it still does something.
	write(L"PobTools", { { L"DataDir", L"D:\\old" }, { L"DataDirHex", L"6162" } });
	{
		LauncherConfig c = LoadLauncherConfig(ini);
		SaveLauncherConfig(ini, c);
		check("T16b the obsolete shared DataDir key is removed on save",
		      read_ini_path(ini, L"DataDir").empty() && read_ini_path(ini, L"DataDirHex").empty());
	}

	// T17 -- the update toggle. Default must be ON: most people want new-season
	// translations, and only someone editing them wants to opt out.
	write(L"PobTools", { { L"Game", L"poe1" } });
	check("T17a UpdateTranslations defaults to on", LoadLauncherConfig(ini).updateTranslations);
	for (int on = 0; on <= 1; on++) {
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.updateTranslations = (on != 0);
		SaveLauncherConfig(ini, c);
		check(on ? "T17b UpdateTranslations round-trips on" : "T17c UpdateTranslations round-trips off",
		      LoadLauncherConfig(ini).updateTranslations == (on != 0));
	}

	// T17d/e -- the proxy field. Default must be empty (= follow the system
	// proxy); a set value round-trips verbatim, normalization is the HTTP
	// layer's job, not the ini's.
	DeleteFileW(ini.c_str());
	write(L"PobTools", { { L"Game", L"poe1" } });
	check("T17d Proxy defaults to empty", LoadLauncherConfig(ini).proxy.empty());
	{
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.proxy = L"127.0.0.1:7890";
		SaveLauncherConfig(ini, c);
		check("T17e Proxy round-trips", LoadLauncherConfig(ini).proxy == L"127.0.0.1:7890");
	}

	// T17f..T17j -- launcher font size (whole-UI zoom) and the remembered window
	// sizes. The clamps are the whole point: a hand-edited or non-numeric value
	// must land on the default, never on 0 px text or a 0x0 window.
	DeleteFileW(ini.c_str());
	write(L"PobTools", { { L"Game", L"poe1" } });
	{
		const LauncherConfig c = LoadLauncherConfig(ini);
		check("T17f FontSize defaults to 19", c.fontSize == kLauncherFontSizeDefault,
		      std::to_string(c.fontSize));
		check("T17f window sizes default to 0 (= mode default)",
		      c.winW == 0 && c.winH == 0 && c.tabWinW == 0 && c.tabWinH == 0);
	}
	{
		// Out of range / garbage -> default; in range -> kept verbatim.
		const struct { const wchar_t* raw; const char* label; int want; } cases[] = {
			{ L"5", "5", 19 }, { L"99", "99", 19 }, { L"abc", "abc", 19 }, { L"-3", "-3", 19 },
			{ L"0", "0", 19 }, { L"14", "14", 14 }, { L"22", "22", 22 }, { L"26", "26", 26 },
		};
		for (const auto& tc : cases) {
			write(L"PobTools", { { L"Game", L"poe1" }, { L"FontSize", tc.raw } });
			const int got = LoadLauncherConfig(ini).fontSize;
			const std::string name = std::string("T17g FontSize \"") + tc.label + "\" -> " +
			                         std::to_string(tc.want);
			check(name.c_str(), got == tc.want, "got " + std::to_string(got));
		}
	}
	{
		const struct { const wchar_t* w; const wchar_t* h; const char* label; int wantW; int wantH; } cases[] = {
			{ L"300",   L"800",  "300x800 (below min)",   0, 0 },
			{ L"20000", L"800",  "20000x800 (above max)", 0, 0 },
			{ L"abc",   L"800",  "abcx800 (text)",        0, 0 },
			{ L"-1",    L"800",  "-1x800",                0, 0 },
			{ L"1280",  nullptr, "1280 with no height",   0, 0 },   // half a pair is no pair
			{ L"1280",  L"800",  "1280x800",              1280, 800 },
		};
		for (const auto& tc : cases) {
			if (tc.h) write(L"PobTools", { { L"Game", L"poe1" }, { L"WindowW", tc.w }, { L"WindowH", tc.h } });
			else      write(L"PobTools", { { L"Game", L"poe1" }, { L"WindowW", tc.w } });
			const LauncherConfig c = LoadLauncherConfig(ini);
			const std::string name = std::string("T17h Window ") + tc.label + " -> " +
			                         std::to_string(tc.wantW) + "x" + std::to_string(tc.wantH);
			check(name.c_str(), c.winW == tc.wantW && c.winH == tc.wantH,
			      "got " + std::to_string(c.winW) + "x" + std::to_string(c.winH));
		}
	}
	{
		DeleteFileW(ini.c_str());
		LauncherConfig c;
		c.fontSize = 22;
		c.winW = 1280;    c.winH = 800;
		c.tabWinW = 1600; c.tabWinH = 1000;
		SaveLauncherConfig(ini, c);
		const LauncherConfig r = LoadLauncherConfig(ini);
		check("T17i FontSize and both window pairs round-trip",
		      r.fontSize == 22 && r.winW == 1280 && r.winH == 800 &&
		          r.tabWinW == 1600 && r.tabWinH == 1000);
	}
	{
		// The two modes remember independently: setting one pair leaves the other
		// at its default in both directions.
		DeleteFileW(ini.c_str());
		write(L"PobTools", { { L"Game", L"poe1" }, { L"TabbedWindowW", L"1600" }, { L"TabbedWindowH", L"1000" } });
		LauncherConfig c = LoadLauncherConfig(ini);
		check("T17j tabbed size alone leaves the separate size at default",
		      c.winW == 0 && c.winH == 0 && c.tabWinW == 1600 && c.tabWinH == 1000);
		DeleteFileW(ini.c_str());
		write(L"PobTools", { { L"Game", L"poe1" }, { L"WindowW", L"1280" }, { L"WindowH", L"800" } });
		c = LoadLauncherConfig(ini);
		check("T17j separate size alone leaves the tabbed size at default",
		      c.winW == 1280 && c.winH == 800 && c.tabWinW == 0 && c.tabWinH == 0);
	}

	// T18..T25 -- ResolveDictDir against synthetic folders. Every failure mode has
	// to land on the built-in folder: a bad setting must never leave POB with no
	// translations at all.
	{
		const std::wstring box = std::wstring(tmp) + L"pobtools_dd_st\\";
		auto rmtree = [](const std::wstring& d) { remove_tree(d); };
		auto mkdir_p = [](const std::wstring& d) { CreateDirectoryW(d.c_str(), nullptr); };
		auto put = [](const std::wstring& p, const std::string& body) {
			HANDLE fh = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			                        FILE_ATTRIBUTE_NORMAL, nullptr);
			if (fh == INVALID_HANDLE_VALUE) return;
			DWORD w = 0;
			WriteFile(fh, body.data(), (DWORD)body.size(), &w, nullptr);
			CloseHandle(fh);
		};

		rmtree(box);
		mkdir_p(box);
		// the "install": poe1's built-in dictionary loads two files
		mkdir_p(box + L"exe");
		mkdir_p(box + L"exe\\Data");
		mkdir_p(box + L"exe\\Data\\poe1");
		mkdir_p(box + L"exe\\Data\\poe1\\zh-rTW");
		put(box + L"exe\\Data\\poe1\\zh-rTW\\meta.json",
		    "{\"load_order\":[\"a.json\",\"b.json\"]}");
		const std::wstring exe = box + L"exe\\";
		const std::wstring builtin1 = exe + L"Data\\poe1\\";

		// a usable external copy of poe1 whose meta.json predates b.json
		mkdir_p(box + L"ext");
		mkdir_p(box + L"ext\\zh-rTW");
		put(box + L"ext\\zh-rTW\\meta.json", "{\"load_order\":[\"a.json\"]}");
		put(box + L"ext\\zh-rTW\\a.json", "{\"entries\":{}}");

		DictDirInfo b0 = ResolveDictDir(exe, DictSlot::Poe1, L"");
		check("T18 empty setting resolves to the built-in folder",
		      b0.status == DataDirStatus::Builtin && b0.root == builtin1 && !b0.insideInstall);

		// Each slot has its own built-in folder -- this is the whole point of
		// splitting them, so it is asserted rather than assumed.
		check("T18b each slot resolves to its own built-in folder",
		      ResolveDictDir(exe, DictSlot::Poe2, L"").root == exe + L"Data\\poe2\\" &&
		          ResolveDictDir(exe, DictSlot::Launcher, L"").root == exe + L"Data\\launcher\\");

		DictDirInfo miss = ResolveDictDir(exe, DictSlot::Poe1, box + L"no_such_folder");
		check("T19 missing folder falls back to built-in and says Missing",
		      miss.status == DataDirStatus::Missing && miss.root == builtin1);

		mkdir_p(box + L"barren");
		DictDirInfo empty = ResolveDictDir(exe, DictSlot::Poe1, box + L"barren");
		check("T20 a folder with no <locale>\\meta.json reports NoDictionaries",
		      empty.status == DataDirStatus::NoDictionaries && empty.root == builtin1);

		// One level too DEEP: the user picked the zh-rTW folder itself.
		DictDirInfo deep = ResolveDictDir(exe, DictSlot::Poe1, box + L"ext\\zh-rTW");
		check("T21 pointing at the locale folder reports WrongShape and suggests going up",
		      deep.status == DataDirStatus::WrongShape && deep.root == builtin1 &&
		          deep.suggestion == box + L"ext\\",
		      narrow_utf8(deep.suggestion));

		// One level too SHALLOW: the user picked the Data folder, which holds poe1\.
		DictDirInfo shallow = ResolveDictDir(exe, DictSlot::Poe1, exe + L"Data");
		check("T21b pointing at the Data folder reports TooShallow and suggests going down",
		      shallow.status == DataDirStatus::TooShallow && shallow.root == builtin1 &&
		          shallow.suggestion == exe + L"Data\\poe1\\",
		      narrow_utf8(shallow.suggestion));

		DictDirInfo ok = ResolveDictDir(exe, DictSlot::Poe1, box + L"ext");
		check("T22 a correctly shaped external copy is used",
		      ok.status == DataDirStatus::External && ok.root == box + L"ext\\" &&
		          ok.found.size() == 1 && ok.found[0].first == "zh-rTW" && ok.found[0].second == 2,
		      ok.found.empty() ? "none found"
		                       : (ok.found[0].first + " n=" + std::to_string(ok.found[0].second)));
		check("T23 a dictionary the external meta.json does not list is reported",
		      ok.staleLoadOrder.size() == 1 && ok.staleLoadOrder[0] == "zh-rTW: b.json",
		      ok.staleLoadOrder.empty() ? "none" : ok.staleLoadOrder[0]);

		// The same folder read as a DIFFERENT slot has no built-in counterpart, so
		// there is nothing to be stale against -- proves the comparison is per slot
		// and not against some shared root.
		DictDirInfo asLauncher = ResolveDictDir(exe, DictSlot::Launcher, box + L"ext");
		check("T23b staleness is compared against the slot's own built-in folder",
		      asLauncher.status == DataDirStatus::External && asLauncher.staleLoadOrder.empty());

		// insideInstall drives the "updates will overwrite this" warning, which is
		// the only thing standing between a translator and a silently clobbered copy.
		DictDirInfo in = ResolveDictDir(exe, DictSlot::Poe1, builtin1);
		check("T24 an external folder under the install is flagged as overwritable",
		      in.insideInstall && !ok.insideInstall);

		// T25 -- the copy button has to produce a layout ResolveDictDir accepts,
		// end to end. Anything less and the feature needs a manual explanation of
		// which folder level to point at, which is exactly what it exists to avoid.
		{
			const std::wstring fresh = box + L"copied\\";
			check("T25a nothing there before the copy", !DictionariesPresentAt(fresh));
			std::string cerr;
			int n = CopyBuiltinDictionary(exe, DictSlot::Poe1, fresh, &cerr);
			DictDirInfo made = ResolveDictDir(exe, DictSlot::Poe1, fresh);
			check("T25b the copied dictionary resolves as a usable external folder",
			      n > 0 && made.status == DataDirStatus::External &&
			          made.found.size() == 1 && made.staleLoadOrder.empty(),
			      n < 0 ? cerr : ("files=" + std::to_string(n)));
			check("T25c the copy is detected as populated afterwards", DictionariesPresentAt(fresh));
		}

		// T27..T29 -- the language list comes from disk. Before this, the two
		// hardcoded strings made every added folder unreachable, so these assert
		// discovery rather than a fixed list.
		{
			// poe1 gets ja-JP (with a display name), poe2 gets ko-KR (without one).
			mkdir_p(box + L"exe\\Data\\poe1\\ja-JP");
			put(box + L"exe\\Data\\poe1\\ja-JP\\meta.json",
			    "{\"display_name\":\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\",\"load_order\":[]}");
			mkdir_p(box + L"exe\\Data\\poe2");
			mkdir_p(box + L"exe\\Data\\poe2\\ko-KR");
			put(box + L"exe\\Data\\poe2\\ko-KR\\meta.json", "{\"load_order\":[]}");

			LauncherConfig lc; // all three slots built-in
			std::vector<LocaleInfo> ls = ListInstalledLocales(exe, lc);

			// en is not a folder -- it is what the engine shows with no dictionary,
			// so it can only come from being put there deliberately.
			check("T27 en is always offered and comes first",
			      !ls.empty() && ls[0].id == "en",
			      ls.empty() ? "empty" : ls[0].id);

			auto find = [&](const char* id) -> const LocaleInfo* {
				for (const LocaleInfo& l : ls) if (l.id == id) return &l;
				return nullptr;
			};
			const LocaleInfo* ja = find("ja-JP");
			const LocaleInfo* ko = find("ko-KR");
			const LocaleInfo* zh = find("zh-rTW");
			// UNION, not intersection: ja-JP exists only for poe1 and must still be
			// offered (poe2 then behaves exactly as it does for en).
			check("T28 languages are the union across the dictionary sets, tagged per set",
			      ja && ko && zh &&
			          ja->slot[0] && !ja->slot[1] &&
			          !ko->slot[0] && ko->slot[1] &&
			          zh->slot[0] && !zh->slot[1],
			      std::to_string(ls.size()) + " found");

			check("T29 display_name is used when present, folder name when not",
			      ja && ko && ja->displayName == "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" &&
			          ko->displayName == "ko-KR",
			      ja ? ja->displayName : "no ja");

			// A language whose folder was deleted must land somewhere sensible.
			check("T29b an unknown configured language falls back to zh-rTW",
			      ls[PickLocaleIndex(ls, L"xx-YY")].id == "zh-rTW");
			check("T29c a known one is picked exactly",
			      ls[PickLocaleIndex(ls, L"ja-JP")].id == "ja-JP" &&
			          ls[PickLocaleIndex(ls, L"en")].id == "en");
			{
				// ...and with no zh-rTW at all, en is the last resort rather than
				// an out-of-range index.
				std::vector<LocaleInfo> only = { ls[0] };
				check("T29d with no zh-rTW installed it falls back to en",
				      only[PickLocaleIndex(only, L"zh-rTW")].id == "en");
			}

			// An external dictionary folder's languages must be listed too --
			// otherwise moving a set out of the install hides its languages.
			LauncherConfig ext;
			ext.dataDir[(int)DictSlot::Poe1] = box + L"ext";
			std::vector<LocaleInfo> le = ListInstalledLocales(exe, ext);
			bool jaGone = true;
			for (const LocaleInfo& l : le) if (l.id == "ja-JP") jaGone = false;
			check("T29e an external folder's languages are listed instead of the built-in ones",
			      jaGone && le.size() >= 2, std::to_string(le.size()) + " found");
		}

		// T26 -- three independent paths. Setting one must not disturb the others,
		// which is the entire behavioural difference from the single shared root.
		{
			DeleteFileW(ini.c_str());
			LauncherConfig c;
			c.dataDir[(int)DictSlot::Poe2] = box + L"ext";
			SaveLauncherConfig(ini, c);
			LauncherConfig back = LoadLauncherConfig(ini);
			check("T26 the three dictionary paths are stored independently",
			      back.dataDir[(int)DictSlot::Poe1].empty() &&
			          back.dataDir[(int)DictSlot::Poe2] == box + L"ext" &&
			          back.dataDir[(int)DictSlot::Launcher].empty());
			check("T26b only the configured slot goes external",
			      ResolveDictDir(exe, DictSlot::Poe1, back.dataDir[0]).status == DataDirStatus::Builtin &&
			          ResolveDictDir(exe, DictSlot::Poe2, back.dataDir[1]).status == DataDirStatus::External);
		}

		rmtree(box);
	}

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\launcher_config_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	DeleteFileW(ini.c_str());
	return failures ? 2 : 0;
}
