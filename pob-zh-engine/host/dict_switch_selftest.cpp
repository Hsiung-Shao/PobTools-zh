// --dict-switch-selftest: the settings page's dictionary switches (kOptionalDicts)
// reach the engine, and the engine honours them.
//
// Three links, each checked on the real code rather than a re-implementation:
//   1. pob-zh.ini <-> LauncherConfig.optionalDictOn (absent key = on).
//   2. LauncherConfig -> POB_ZH_DICT_SWITCHES (DictSwitchesEnv), the value
//      SetEngineEnv hands the engine.
//   3. the engine's loader (translation_reload + translation_lookup) against a
//      throwaway dictionary in %TEMP%: switched on, the file loads and, being last
//      in load_order, wins; switched off, it is skipped and nothing else changes.
// Plus the shipped data: Data\poe1\zh-rTW\meta.json lists ui_tooltips.json LAST
// and the file is there.
//
// Never writes the install's own dictionaries or pob-zh.ini.

#include "dict_switch_selftest.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "launcher_config.h"
#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	(ok ? g_pass : g_fail)++;
}

void put(const std::wstring& p, const std::string& body)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD w = 0;
	WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
	CloseHandle(h);
}

std::string slurp(const std::wstring& p)
{
	std::string out;
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return out;
	char buf[8192];
	DWORD n = 0;
	while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
	CloseHandle(h);
	return out;
}

std::string say(const char* en)
{
	const char* r = translation_lookup(en);
	return r ? r : "";
}

int tooltip_index()
{
	for (int i = 0; i < kOptionalDictCount; i++)
		if (std::string(kOptionalDicts[i].file) == "ui_tooltips.json") return i;
	return -1;
}

} // namespace

int RunDictSwitchSelfTest(const std::wstring& exeDir)
{
	printf("=== dictionary switches (ui_tooltips.json) ===\n");
	const int ti = tooltip_index();
	check(ti >= 0, "S1 ui_tooltips.json 是可切換的字典");
	if (ti < 0) {
		printf("\ndict switch selftest: %d passed, %d failed\n", g_pass, g_fail);
		return 3;
	}

	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring box = std::wstring(tmp) + L"pobtools_dict_switch\\";
	const std::wstring leaf = box + L"zh-rTW\\";
	CreateDirectoryW(box.c_str(), nullptr);
	CreateDirectoryW(leaf.c_str(), nullptr);

	// --- 1. ini <-> config ----------------------------------------------------
	printf("\n--- pob-zh.ini ---\n");
	{
		const std::wstring ini = box + L"pob-zh.ini";
		DeleteFileW(ini.c_str());
		check(LoadLauncherConfig(ini).optionalDictOn[ti] == 1, "S2 ini 沒有這個鍵時預設為開");
		LauncherConfig c = LoadLauncherConfig(ini);
		c.optionalDictOn[ti] = 0;
		SaveLauncherConfig(ini, c);
		check(LoadLauncherConfig(ini).optionalDictOn[ti] == 0, "S3 關掉後存檔再讀回仍是關");
		c.optionalDictOn[ti] = 1;
		SaveLauncherConfig(ini, c);
		check(LoadLauncherConfig(ini).optionalDictOn[ti] == 1, "S4 再打開存檔讀回是開");
		DeleteFileW(ini.c_str());
	}

	// --- 2. config -> env -------------------------------------------------------
	printf("\n--- POB_ZH_DICT_SWITCHES ---\n");
	{
		LauncherConfig c;
		check(DictSwitchesEnv(c) == L"ui_tooltips.json=1", "S5 預設設定傳給引擎 ui_tooltips.json=1");
		c.optionalDictOn[ti] = 0;
		check(DictSwitchesEnv(c) == L"ui_tooltips.json=0", "S6 關掉時傳給引擎 ui_tooltips.json=0");
	}

	// --- 3. the engine's loader ---------------------------------------------
	printf("\n--- engine loader ---\n");
	{
		// base.json and ui_tooltips.json share one key; the tooltip file is last,
		// so when it loads its value wins. One key exists only in each file.
		const char* kShared = "PobToolsDictSwitchShared";
		const char* kBaseOnly = "PobToolsDictSwitchBaseOnly";
		const char* kTipOnly = "PobToolsDictSwitchTipOnly";
		put(leaf + L"meta.json", "{\"load_order\":[\"base.json\",\"ui_tooltips.json\"]}");
		put(leaf + L"base.json", std::string("{\"entries\":{\"") + kShared + "\":\"BASE\",\"" + kBaseOnly + "\":\"BASEONLY\"}}");
		put(leaf + L"ui_tooltips.json", std::string("{\"entries\":{\"") + kShared + "\":\"TIP\",\"" + kTipOnly + "\":\"TIPONLY\"}}");

		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", box.c_str());

		SetEnvironmentVariableW(L"POB_ZH_DICT_SWITCHES", L"ui_tooltips.json=1");
		translation_reload();
		const int countOn = translation_get_count();
		check(say(kShared) == "TIP", "S7 開:ui_tooltips.json 載入且排最後勝出");
		check(say(kTipOnly) == "TIPONLY", "S8 開:只在 ui_tooltips.json 的鍵查得到");
		check(say(kBaseOnly) == "BASEONLY", "S9 開:其他字典照常");

		SetEnvironmentVariableW(L"POB_ZH_DICT_SWITCHES", L"ui_tooltips.json=0");
		translation_reload();
		const int countOff = translation_get_count();
		check(say(kShared) == "BASE", "S10 關:共用鍵回到前面字典的值");
		check(say(kTipOnly).empty(), "S11 關:ui_tooltips.json 的鍵查不到(沒載入)");
		check(say(kBaseOnly) == "BASEONLY", "S12 關:其他字典照常");
		check(countOn - countOff == 1, "S13 關:總筆數只少 ui_tooltips.json 獨有的那一筆");
		if (countOn - countOff != 1) printf("           on=%d off=%d\n", countOn, countOff);

		// No variable at all (an older launcher, the CLI path): everything loads.
		SetEnvironmentVariableW(L"POB_ZH_DICT_SWITCHES", nullptr);
		translation_reload();
		check(say(kShared) == "TIP", "S14 沒有這個環境變數時照 load_order 全部載入");

		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
		translation_reload();
		DeleteFileW((leaf + L"meta.json").c_str());
		DeleteFileW((leaf + L"base.json").c_str());
		DeleteFileW((leaf + L"ui_tooltips.json").c_str());
	}
	RemoveDirectoryW(leaf.c_str());
	RemoveDirectoryW(box.c_str());

	// --- 4. shipped data ----------------------------------------------------
	printf("\n--- shipped Data\\poe1\\zh-rTW ---\n");
	{
		const std::wstring dir = exeDir + L"Data\\poe1\\zh-rTW\\";
		const std::string meta = slurp(dir + L"meta.json");
		const size_t lo = meta.find("\"load_order\"");
		const size_t close = lo == std::string::npos ? std::string::npos : meta.find(']', lo);
		const size_t last = close == std::string::npos ? std::string::npos : meta.rfind('"', close);
		const size_t first = last == std::string::npos || last == 0 ? std::string::npos : meta.rfind('"', last - 1);
		const std::string lastName = first == std::string::npos ? "" : meta.substr(first + 1, last - first - 1);
		check(lastName == "ui_tooltips.json", "S15 出貨 meta.json 的 load_order 最後一個是 ui_tooltips.json");
		if (lastName != "ui_tooltips.json") printf("           last = '%s'\n", lastName.c_str());
		const std::string tip = slurp(dir + L"ui_tooltips.json");
		check(tip.find("\"entries\"") != std::string::npos, "S16 出貨 ui_tooltips.json 存在且有 entries");
	}

	printf("\ndict switch selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 3;
}
