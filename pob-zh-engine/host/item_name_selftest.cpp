// Headless regression for POB's generated item names.
//
// POB has no i18n layer: a rare or cluster jewel is named by concatenation at runtime
// (Item.lua:1572 `self.name = self.title .. ", " .. self.baseName`), so the finished string is
// never a dictionary key. Three translation_lookup paths handle the result:
//
//   uniques        "Megalomaniac, Medium Cluster Jewel"      exact table, whole name
//   magic items    "Examiner's Diamond Flask of Penetrating" glossary, multi-word terms
//   rares/clusters "Loath Joy, Medium Cluster Jewel"         glossary, bare word pair
//
// The glossary refuses keys shorter than four characters (is_glossary_candidate) so short function
// words cannot make an English sentence reach the full-coverage bar; 26 word roots are below that
// (Joy, Eye, Orb ...), which left the ENTIRE name English. The floor is relaxed only after the last
// comma-separated segment is confirmed to be a real base type, so prose can never reach it.
//
// The fixtures are one build's equipment and ten jewel sockets captured in both English and
// Traditional Chinese (observed, not invented); the SIXTEEN names that already worked are a
// regression guard and must stay byte-identical.

#include "item_name_selftest.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
}

// Distinguishable from a legitimately empty translation.
const char* kNull = "\x01<null>";

std::string fwd(const char* en)
{
	const char* r = translation_lookup(en);
	return r ? std::string(r) : std::string(kNull);
}

void expect(const char* en, const char* zh)
{
	std::string got = fwd(en);
	bool ok = (got == zh);
	printf("  %s  %-38s -> %s\n", ok ? "[PASS]" : "[FAIL]", en, got.c_str());
	if (!ok) printf("           expected: %s\n", zh);
	(ok ? g_pass : g_fail)++;
}

// The relaxed per-word path must not fire. Staying English here is correct;
// anything translated means the base-type boundary leaked.
void expect_untranslated(const char* en, const char* why)
{
	std::string got = fwd(en);
	bool ok = (got == kNull);
	printf("  %s  %-38s -> %s\n", ok ? "[PASS]" : "[FAIL]", en,
	       ok ? "(untranslated, correct)" : got.c_str());
	if (!ok) printf("           LEAK: %s\n", why);
	(ok ? g_pass : g_fail)++;
}

// The tooltip path: the caller has proved this is a title, so no base type is
// needed and the glossary floor does not apply.
void expect_title(const char* en, const char* zh)
{
	const char* r = translation_lookup_item_title(en);
	std::string got = r ? std::string(r) : std::string(kNull);
	bool ok = (got == zh);
	printf("  %s  title %-32s -> %s\n", ok ? "[PASS]" : "[FAIL]", en, got.c_str());
	if (!ok) printf("           expected: %s\n", zh);
	(ok ? g_pass : g_fail)++;
}

// The Lua source patch degrades silently when its anchor drifts (ui_api.cpp loads the module
// unmodified), so the anchor is asserted. Both shipped POB folders are checked when present.
int count_occurrences(const std::string& hay, const std::string& needle)
{
	int n = 0;
	for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
	return n;
}

// The Lua source patches degrade silently (ui_api.cpp loads the module unpatched when an anchor
// no longer matches); these assertions are the alarm.
//
// Checked per FILE, not per anchor: PoE1 and PoE2 differ, so each variant's anchor is absent from
// the other folder. Every file present on disk must be reached by at least one of its anchors.
struct AnchorExpect {
	const char* relPath;    // path under the POB folder, e.g. "Classes\\ItemsTab.lua"
	const char* anchor;
	// Insert-mode patches are skipped unless they match exactly once.
	bool insertMode;
};

void check_patch_anchor(const std::string& exeDir)
{
	static const char* kFolders[] = {
		"PathOfBuildingCommunity", "PathOfBuildingCommunity-PoE2-Portable"
	};
	static const AnchorExpect kAnchors[] = {
		// Two shapes of the same feature: PoE1 builds a searchName, PoE2 matches
		// item.name inline. Each matches in one folder only.
		{ "Classes\\ItemDBControl.lua", "local searchName = item.name:lower()",     true  },
		{ "Classes\\ItemDBControl.lua", "string.matchOrPattern, item.name:lower()", true  },
		{ "Classes\\ItemsTab.lua",      "rarityCode..item.title",                   true  },
		{ "Classes\\TreeTab.lua",       "label = node.dn .. \"",                    false },
		// PoE2 has no tattoos, so this one is absent there; the file is still
		// covered by the label anchor above.
		{ "Classes\\TreeTab.lua",       "table.concat(node.sd, \",\")",             true  },
		{ "Modules\\Main.lua",          "function main:WrapString(str, height, width)", true },
	};

	int filesScanned = 0;
	for (const char* folder : kFolders) {
		for (const AnchorExpect& probe : kAnchors) {
			bool firstForFile = true;
			for (const AnchorExpect& a : kAnchors) {
				if (&a == &probe) break;
				if (strcmp(a.relPath, probe.relPath) == 0) { firstForFile = false; break; }
			}
			if (!firstForFile) continue;

			std::string path = exeDir + folder + "\\" + probe.relPath;
			std::ifstream f(path, std::ios::binary);
			if (!f) continue;
			std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			filesScanned++;

			int total = 0;
			for (const AnchorExpect& a : kAnchors) {
				if (strcmp(a.relPath, probe.relPath) != 0) continue;
				int n = count_occurrences(src, a.anchor);
				total += n;
				if (a.insertMode && n > 1) {
					char what[320];
					snprintf(what, sizeof(what), "%s/%s: \"%.40s\" is ambiguous (%d matches, insert patches need one)",
					         folder, a.relPath, a.anchor, n);
					check(false, what);
				}
			}
			char what[256];
			snprintf(what, sizeof(what), "%s/%s: reached by %d anchor match(es)",
			         folder, probe.relPath, total);
			check(total >= 1, what);
		}
	}
	// Zero files means every assertion above was skipped. Say so rather than pass.
	check(filesScanned > 0, "at least one shipped POB source file was actually scanned");
}

} // namespace

int RunItemNameSelftest()
{
	SetConsoleOutputCP(CP_UTF8);
	// Pin the locale so the result does not depend on the surrounding install's language.
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	translation_init();
	printf("item name selftest: locale=%s entries=%d\n",
	       translation_get_locale() ? translation_get_locale() : "(null)",
	       translation_get_count());

	printf("\n-- the defect: a title word below the glossary floor --\n");
	// "Joy" is three letters (below the glossary floor).
	expect("Loath Joy, Medium Cluster Jewel", "惡行 歡愉, 中型星團珠寶");

	printf("\n-- regression guard: titles at or above the floor, unchanged --\n");
	// Four characters exactly (at the floor).
	expect("Soul Star, Viridian Jewel", "靈魂 之星, 翠綠珠寶");
	expect("Rift Suit, Twilight Regalia", "術痕 戰服, 暮光法衣");
	expect("Blood Goad, Kinetic Wand", "鮮血 風喚, 動能魔杖");
	expect("Damnation Wound, Large Cluster Jewel", "咒縛 傷口, 巨型星團珠寶");
	expect("Damnation Desire, Cobalt Jewel", "咒縛 渴望, 鈷藍珠寶");
	expect("Rapture Curio, Medium Cluster Jewel", "狂喜 逸品, 中型星團珠寶");
	expect("Ghoul Vessel, Large Cluster Jewel", "食屍鬼 之器, 巨型星團珠寶");
	expect("Dragon Spark, Cobalt Jewel", "巨龍 電球, 鈷藍珠寶");
	expect("Kraken Twirl, Helical Ring", "海怪 之輪, 螺旋戒指");
	expect("Havoc Chant, Opal Wand", "禍害 聖曲, 靈石法杖");
	expect("Morbid Star, Titanium Spirit Shield", "災病 之星, 巨人魔盾");

	printf("\n-- regression guard: uniques still resolve as whole names --\n");
	// These hit the exact table; identical output proves they never go through the per-word path.
	expect("Megalomaniac, Medium Cluster Jewel", "妄想症, 中型星團珠寶");
	expect("Watcher's Eye, Prismatic Jewel", "看守之眼, 三相珠寶");
	expect("Healthy Mind, Cobalt Jewel", "靈體轉換, 鈷藍珠寶");
	expect("Cinderswallow Urn, Silver Flask", "噬燼甕, 真銀藥劑");
	expect("Oriath's End, Bismuth Flask", "奧瑞亞之終, 灰岩藥劑");

	printf("\n-- the boundary: no base type, no relaxation --\n");
	// Same title as the fixed case; only the tail differs: shows the base-type check gates the relaxation.
	expect_untranslated("Loath Joy, Nonexistent Basetype",
	                    "relaxed path fired without a real base type in the tail");
	// Prose that happens to end in a base type. Not every title word is a
	// dictionary key, so all-or-nothing must reject it.
	expect_untranslated("You can drag it here, Cobalt Jewel",
	                    "relaxed path accepted a title whose words are not all keys");

	printf("\n-- tooltip path: the title alone, with no base type beside it --\n");
	expect_title("Loath Joy", "惡行 歡愉");        // below the floor
	expect_title("Rapture Curio", "狂喜 逸品");    // above it, unchanged
	expect_title("Megalomaniac", "妄想症");        // unique, via the exact table

	printf("\n-- title words vs UI words: GGPK words table in titles, clientstrings elsewhere --\n");
	// "Keep" is 堅城 in a rare name (words) but 保留 as a button (clientstrings).
	expect_title("Cataclysm Keep", "災變 堅城");
	expect_title("Cataclysm Hide", "災變 防身甲");
	expect("Cataclysm Keep, Titanium Spirit Shield", "災變 堅城, 巨人魔盾");
	expect("Keep", "保留");
	expect("Hide", "隱藏");
	// "Pause" bare is the keyboard key in POB's help list; GGPK has no key name, so it stays English.
	expect_title("Cataclysm Pause", "災變 歇息");
	expect_untranslated("Pause", "bare Pause (help-list key name) was translated");

	printf("\n-- isolation: the relaxed answer must not escape into the general path --\n");
	// Called AFTER the titles above: proves translation_lookup_item_title's separate cache keeps
	// the relaxed answer from reaching later callers.
	expect_untranslated("Loath Joy",
	                    "translation_lookup returned a title-only result");

	printf("\n-- tooltip lines POB formats at runtime --\n");
	// ItemsTab.lua:4615 builds this with s_format("Intangibility: ^7%d%%", n), never a dictionary key
	// as written; the pattern map handles it only because ui.json carries the POB-side wording (GGPK
	// writes the term only inside "[Key|Display]" markup). 無形性 is the official term (keywordpopups.Term).
	expect("Intangibility: 6%", "無形性: 6%");
	expect("Intangibility: 100%", "無形性: 100%");

	printf("\n-- the Lua patch anchor still matches upstream --\n");
	{
		char exePath[MAX_PATH];
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string dir(exePath);
		size_t slash = dir.find_last_of('\\');
		dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash + 1);
		check_patch_anchor(dir);
	}

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

int RunTranslateProbe(const std::wstring& text, const std::wstring& locale)
{
	SetConsoleOutputCP(CP_UTF8);
	// Default zh-rTW; `--tr "<english>" ko-KR` probes another installed language.
	std::string loc = "zh-rTW";
	if (!locale.empty()) {
		int ln = WideCharToMultiByte(CP_UTF8, 0, locale.c_str(), (int)locale.size(), nullptr, 0, nullptr, nullptr);
		loc.assign(ln > 0 ? ln : 0, '\0');
		if (ln > 0) WideCharToMultiByte(CP_UTF8, 0, locale.c_str(), (int)locale.size(), &loc[0], ln, nullptr, nullptr);
	}
	SetEnvironmentVariableA("POB_LOCALE", loc.c_str());
	// Wall time since process creation (shows the exe cost BEFORE the dictionaries are touched).
	auto sinceStart = []() -> double {
		FILETIME c, e, k, u;
		GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		ULARGE_INTEGER a, b;
		a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
		b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
		return (double)(b.QuadPart - a.QuadPart) / 10000.0;
	};
	const double tBefore = sinceStart();
	translation_init();
	const double tAfter = sinceStart();
	printf("process->init %.0f ms, init->done %.0f ms\n", tBefore, tAfter - tBefore);

	int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
	std::string en(n, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &en[0], n, nullptr, nullptr);

	printf("locale=%s entries=%d %s\n", translation_get_locale() ? translation_get_locale() : "(null)",
	       translation_get_count(), translation_get_init_stats());
	if (en.empty()) { printf("usage: --tr \"<english>\"\n"); return 2; }

	const char* zh = translation_lookup(en.c_str());
	const char* title = translation_lookup_item_title(en.c_str());
	printf("  in            : %s\n", en.c_str());
	printf("  lookup        : %s\n", zh ? zh : "(no match)");
	printf("  as item title : %s\n", title ? title : "(no match)");
	return zh ? 0 : 1;
}
