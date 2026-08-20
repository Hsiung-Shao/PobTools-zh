// Headless regression for the Chinese item paste path.
//
// The invariant this file exists to defend is blunt: after reverse translation,
// NOTHING may still be non-ASCII. POB runs every pasted item through
// sanitiseText (Classes/Item.lua:68 -> Modules/Common.lua:272), which replaces
// every byte >= 0x80 with a literal '?'. So a line we fail to translate is not
// "left in Chinese" -- it is destroyed, one '?' per byte, and shows up in POB as
// a red "(Not supported in PoB yet)" modifier. "無形性" (9 bytes) became exactly
// "?????????" in the report that prompted this work.
//
// The fixtures are real items the user copied out of 3.29, kept verbatim
// (including GGG's own inconsistencies: 階層 in one annotation and 階級 in the
// next, and the em-dash carrying no leading space in Chinese where the English
// template has one). Six items are enough to cover every shape the reported ten
// contained -- unique flask, rare armour with eldritch implicits, fractured
// jewel, Foulborn unique jewel, helmet with an implicit annotation and a
// negative value, and a cluster jewel with enchant lines.
//
// kKnownGaps is the list of lines allowed to stay Chinese -- genuine dictionary
// gaps, named exactly so they stay visible, since a missing mod that silently
// disappears is worse than one POB flags. It is currently EMPTY: every item
// fixture must come out fully ASCII. Filling a gap therefore fails this test,
// which is the correct prompt to update it. That is not hypothetical -- adding
// "Intangibility: {0}%" to ui.json is what rewrote the property assertions
// below.

#include "paste_selftest.h"
#include "paste_fixtures.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
}

bool has_non_ascii(const std::string& s)
{
	for (unsigned char c : s) if (c >= 0x80) return true;
	return false;
}

std::vector<std::string> split_lines(const std::string& s)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t eol = s.find('\n', pos);
		if (eol == std::string::npos) { out.push_back(s.substr(pos)); break; }
		out.push_back(s.substr(pos, eol - pos));
		pos = eol + 1;
	}
	return out;
}

// Runs the real public entry point. Returns "" when nothing translated, so a
// caller can tell "dropped every line" from "refused to touch it".
std::string rev(const char* zh)
{
	char* out = translation_reverse_text(zh);
	if (!out) return std::string("\x01<null>");
	std::string s(out);
	translation_free(out);
	return s;
}

// Convenience for single-line assertions. Trailing newline is stripped.
std::string rev_line(const char* zh)
{
	std::string s = rev(zh);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	return s;
}

// Puts one line inside the property block of an otherwise ordinary rare item and
// reports whether anything of it came out. A bare line cannot answer this: the
// property rule is section-driven, so outside an item there is no property block
// and the line is deliberately left alone.
bool prop_survives(const char* propLine, const char* expectContains = nullptr)
{
	std::string item = "物品種類: 珠寶\n稀有度: 稀有\n末日 電球\n赤紅珠寶\n--------\n";
	item += propLine;
	item += "\n--------\n物品等級: 69\n--------\n"
	        "{ 前綴 \"焚燒之\"(階層：1)— 傷害 }\n增加 19(16-20)% 燃燒傷害\n";
	std::string out = rev(item.c_str());
	if (expectContains) return out.find(expectContains) != std::string::npos;
	// nothing of the Chinese may survive, and no English stand-in either
	return out.find(propLine) != std::string::npos;
}

// Forward direction (English -> Chinese), the one POB's own display uses.
bool fwd_eq(const char* en, const char* expect)
{
	const char* got = translation_lookup(en);
	if (!got) { printf("           FWD '%s' -> (NOT FOUND)\n", en); return false; }
	if (std::string(got) == expect) return true;
	printf("           FWD '%s'\n             got:    '%s'\n             expect: '%s'\n", en, got, expect);
	return false;
}

// One "<kind>\t<line>" row per line, so a fixture's section labels can be
// asserted directly instead of inferred from the English that came out.
std::vector<std::string> kinds_of(const char* zh)
{
	std::vector<std::string> out;
	for (const std::string& row : split_lines(translation_classify_dump(zh))) {
		size_t tab = row.find('\t');
		if (tab != std::string::npos) out.push_back(row.substr(0, tab));
	}
	return out;
}

// "header*4 sep property*6" -> "header header header header sep property ..."
// The run-length form is what makes these assertions readable: an item has 30-40
// lines and a bare wall of repeated words is a diff nobody can check by eye.
std::string expand_kinds(const char* spec)
{
	std::string out;
	std::string tok;
	std::string s = spec;
	s += ' ';
	for (char c : s) {
		if (c != ' ') { tok += c; continue; }
		if (tok.empty()) continue;
		int n = 1;
		size_t star = tok.find('*');
		if (star != std::string::npos) { n = atoi(tok.c_str() + star + 1); tok = tok.substr(0, star); }
		for (int i = 0; i < n; i++) { if (!out.empty()) out += ' '; out += tok; }
		tok.clear();
	}
	return out;
}

// Asserts the whole label sequence of a fixture. Written as one space-separated
// string so a wrong label shows up as a readable diff rather than an index.
void check_kinds(const char* name, const char* zh, const char* spec)
{
	std::string got;
	for (const std::string& k : kinds_of(zh)) { if (!got.empty()) got += ' '; got += k; }
	std::string expect = expand_kinds(spec);
	bool ok = got == expect;
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s: section labels\n", ok ? "[PASS]" : "[FAIL]", name);
	if (!ok) printf("           got:    %s\n           expect: %s\n", got.c_str(), expect.c_str());
}

void check_eq(const char* zh, const char* expect, const char* what)
{
	std::string got = rev_line(zh);
	bool ok = got == expect;
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
	if (!ok) printf("           got:    '%s'\n           expect: '%s'\n", got.c_str(), expect);
}

// fixtures live in paste_fixtures.h (shared with --filter-selftest)

// Lines that legitimately have no translation yet, keyed by the fixture they
// belong to. Asserted exactly so a gap can neither appear nor vanish unnoticed.
//
// Empty since the per-line registration of GGG's own multi-line entries landed:
// the one entry here ("範圍內天賦獲得火焰抗性或全部元素抗性") was never missing
// from the dictionary, it was the FIRST LINE of a two-line entry whose key is
// stored joined, so neither half could ever match on its own.
struct KnownGap { const char* item; const char* line; };
const KnownGap kKnownGaps[] = {
	{ nullptr, nullptr },
};

void check_item(const char* name, const char* raw, int expect_gaps)
{
	std::string out = rev(raw);
	std::vector<std::string> lines = split_lines(out);
	std::vector<std::string> left;
	for (const std::string& l : lines) if (has_non_ascii(l)) left.push_back(l);

	char what[256];
	snprintf(what, sizeof(what), "%s: %d line(s) still non-ASCII (expected %d)",
	         name, (int)left.size(), expect_gaps);
	check((int)left.size() == expect_gaps, what);
	for (const std::string& l : left) {
		bool known = false;
		for (const KnownGap& g : kKnownGaps) if (g.line && l == g.line) known = true;
		if (!known) printf("           UNEXPECTED leftover: '%s'\n", l.c_str());
	}
}

} // namespace

int RunPasteSelftest()
{
	// The fixtures are zh-rTW; pin the locale so the result does not depend on
	// whichever language the surrounding install happens to be set to.
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	translation_init();
	printf("paste selftest: locale=%s entries=%d\n",
	       translation_get_locale() ? translation_get_locale() : "(null)",
	       translation_get_count());

	printf("\n-- whole items: nothing may survive as non-ASCII --\n");
	check_item("unique flask", kFxFlask, 0);
	check_item("rare boots", kFxBoots, 0);
	check_item("fractured jewel", kFxJewel, 0);
	check_item("helmet", kFxHelm, 0);
	check_item("Foulborn jewel", kFxFoulborn, 0);
	check_item("cluster jewel", kFxCluster, 0);
	check_item("large cluster jewel", kFxLargeCluster, 0);
	check_item("Melding of the Flesh", kFxMelding, 0);
	check_item("vestigial boots", kFxVestigialBoots, 0);

	printf("\n-- section grammar: every line's label, on every fixture --\n");
	// Section COUNT and ORDER differ across these eight (6 to 8 sections; sockets,
	// requirements and influence come and go), which is exactly why the grammar
	// keys on the Item Level line and on what a section contains, never on "the
	// third section". These sequences are the proof that it holds up.
	check_kinds("unique flask", kFxFlask,
	            "header*4 sep property*6 sep property*2 sep property sep enchant "
	            "sep mods*10 sep flavour sep desc");
	check_kinds("rare boots", kFxBoots,
	            "header*4 sep property*4 sep property*5 sep property sep property "
	            "sep mods*5 sep mods*15");
	check_kinds("fractured jewel", kFxJewel,
	            "header*4 sep property sep property sep mods*10 sep desc sep status");
	check_kinds("Foulborn jewel", kFxFoulborn,
	            "header*4 sep property*2 sep property sep mods*6 sep flavour sep desc");
	check_kinds("helmet", kFxHelm,
	            "header*4 sep property*4 sep property*5 sep property sep property "
	            "sep mods*3 sep mods*15 sep status");
	check_kinds("cluster jewel", kFxCluster,
	            "header*4 sep property sep property*2 sep property sep enchant*6 "
	            "sep mods*8 sep desc");
	check_kinds("large cluster jewel", kFxLargeCluster,
	            "header*4 sep property sep property*2 sep property sep enchant*6 "
	            "sep mods*8 sep desc");
	check_kinds("Melding of the Flesh", kFxMelding,
	            "header*4 sep property sep property sep mods*7 sep flavour*3 sep desc");
	check_kinds("vestigial boots", kFxVestigialBoots,
	            "header*4 sep property*2 sep property*3 sep property sep property "
	            "sep mods*3 sep mods*12 sep flavour");

	printf("\n-- advanced-copy annotations rebuilt into POB's flag form --\n");
	check_eq("{ 固定詞綴 }", "{ Implicit Modifier }", "implicit");
	check_eq("{ 傳奇詞綴 }", "{ Unique Modifier }", "unique");
	check_eq("{ 傳奇詞綴— 傷害 }", "{ Unique Modifier }", "tags dropped after em-dash");
	// The quoted affix name is what POB matches against its own mod database to
	// fill the item's prefix/suffix slots (Item.lua:463). Dropping it is what
	// made a pasted cluster jewel lose every notable it rolled.
	check_eq("{ 前綴 \"獵豹的\"(階層：2)— 速度 }", "{ Prefix Modifier \"Cheetah's\" }",
	         "prefix keeps the affix name, tier dropped");
	check_eq("{ 已破裂 前綴 \"衰老的\"(階層：1)— 異常狀態 }",
	         "{ Fractured Prefix Modifier \"Decrepifying\" }", "fractured + prefix");
	// GGG writes 階級 here and 階層 above -- both are its own templates.
	check_eq("{ 已大師工藝 前綴 \"進化的\"(階級：3)— 防禦,護甲,能量護盾 }",
	         "{ Master Crafted Prefix Modifier \"Upgraded\" }",
	         "master crafted + prefix (階級 spelling)");
	// A name with more than one English counterpart, or one POB has no affix
	// for, is left off entirely -- the old behaviour. 精髓之 is Essence /
	// of Essences / of the Essence in GGG's own data, so there is nothing to
	// pick from and guessing would put the wrong affix in the slot.
	check_eq("{ 後綴 \"精髓之\"— 傷害,元素,寶石 }", "{ Suffix Modifier }",
	         "an ambiguous affix name is omitted, not guessed");
	// The one that made the cluster jewel fail: all 284 notable modifiers share
	// this single name, and POB narrows down using the stat line that follows.
	check_eq("{ 前綴 \"顯著的\"(階層：1)— 傷害,元素,抗性 }",
	         "{ Prefix Modifier \"Notable\" }", "cluster jewel notable affix name");
	check_eq("{ 灼烙總督固定詞綴 (宏偉)— 元素,閃電,異常狀態 }",
	         "{ Searing Exarch Implicit Modifier }", "eldritch implicit keeps exarch+implicit");
	check_eq("{ 吞噬天地固定詞綴 (宏偉)— 傷害,元素 }",
	         "{ Eater of Worlds Implicit Modifier }", "eldritch implicit keeps eater+implicit");
	// GGG ships this one untranslated; it already parses, so it passes through.
	check_eq("{ Foulborn Unique Modifier— 元素,火焰 }",
	         "{ Foulborn Unique Modifier }", "untranslated annotation passes through");
	// Unrecognised annotations are stripped, never left as Chinese.
	check_eq("{ 這不是任何一種註解 }", "", "unknown annotation is stripped, not left in Chinese");

	printf("\n-- composed lines: translate the parts, not the whole --\n");
	// GGPK stores only "Added Small Passive Skills grant: {0}", so the composed
	// string is in no dictionary; POB declares 60 of them and 52 had no entry.
	check_eq("附加的小型天賦給予：增加 10% 元素傷害 (enchant)",
	         "Added Small Passive Skills grant: 10% increased Elemental Damage (enchant)",
	         "cluster enchant composed from its inner stat");
	check_eq("附加的小型天賦給予：被捷光環影響時，增加 10% 傷害 (enchant)",
	         "Added Small Passive Skills grant: 10% increased Damage while affected by a Herald (enchant)",
	         "same rule, a different inner stat");
	// "grant:" vs "also grant:" is decided by the " (enchant)" suffix, not by the
	// Chinese wording -- GGG writes 小型 on the enchant and 小 on the modifier but
	// uses both for both in its stat data. Verified against POB's own export of
	// the same jewel, which lists the enchant as "grant:" and the rolled suffix
	// as "also grant:". Getting this wrong made POB show the rolled modifier as
	// "(Not supported in PoB yet)".
	check_eq("附加的小天賦給予：+3(2-3)% 冰冷抗性",
	         "Added Small Passive Skills also grant: +3(2-3)% to Cold Resistance",
	         "a rolled modifier gets 'also grant:'");
	check_eq("附加的小型天賦給予：+3(2-3)% 冰冷抗性 (enchant)",
	         "Added Small Passive Skills grant: +3(2-3)% to Cold Resistance (enchant)",
	         "the same stat as an enchant gets 'grant:'");
	// The value must survive the round trip. Without this guard the rule wrapped
	// a wrong inner match and turned a line that used to fail visibly into
	// confident nonsense ("...also grant: you and nearby Allies"); the census
	// caught three of them.
	{
		std::string s = rev("稀有度: 稀有\n狂喜光澤\n巨型星團珠寶\n--------\n"
		                    "附加的小天賦給予：+37% 最大能量護盾\n");
		bool kept = s.find("37") != std::string::npos;
		bool untouched = s.find("附加的小天賦給予") != std::string::npos;
		check(kept || untouched,
		      "composition never drops the value: either 37 survives or the line is left alone");
	}

	// An inner stat that translates to nothing must leave the line alone rather
	// than emit a half-English hybrid. Probed inside a real item because
	// translation_reverse_text reports "nothing translated" as a null return, so
	// on its own an untouched line is indistinguishable from a refusal.
	{
		std::string s = rev("稀有度: 稀有\n狂喜光澤\n巨型星團珠寶\n--------\n"
		                    "附加的小型天賦給予：這不是任何一條詞綴\n");
		check(s.find("附加的小型天賦給予：這不是任何一條詞綴") != std::string::npos,
		      "untranslatable inner leaves the line untouched (no half-English hybrid)");
	}

	printf("\n-- unknown properties: dropped by the rule, not by a list --\n");
	// There is no drop list any more. A "key: value" line inside the property
	// block that nothing could translate is dropped because of WHERE it is, so
	// these four assertions have to hold together: the known case still drops,
	// a key nobody has ever seen drops too, a key POB does consume survives, and
	// the same text outside the property block is left alone.
	check(!prop_survives("虛構屬性: 42%"),
	      "an unknown property key drops -- the rule is not a list");
	check(prop_survives("品質: +20% (augmented)", "Quality: +20%"),
	      "a property POB does consume is NOT dropped");
	// 無形性 was the one hardcoded drop, and then the worked example of the rule
	// that replaced it, purely because the dictionary had no entry for POB's own
	// wording ("Intangibility: {0}%", now in ui.json -- GGPK only ever writes the
	// term inside its "[Key|Display]" markup, which POB does not emit). With a
	// translation available the line no longer reaches the drop rule, and that is
	// the better outcome: POB writes this property itself (Item.lua:1753) and
	// parses it back (Item.lua:825), so a pasted item now KEEPS its Intangibility
	// instead of losing it.
	//
	// Asserted by its English form. "The Chinese vanished" would pass either way
	// -- dropped or translated -- so it would have hidden this change entirely.
	check(prop_survives("無形性: 15%", "Intangibility: 15%"),
	      "Intangibility translates, so it survives as a real property");
	// Outside the property block the same shape must fall through untouched:
	// dropping it there would delete a modifier that merely contains a colon.
	// Sampled with a key that cannot translate, so a future dictionary addition
	// cannot quietly turn this into a vacuous pass the way 無形性 just did.
	{
		std::string s = rev("稀有度: 稀有\n狂喜光澤\n巨型星團珠寶\n--------\n虛構屬性: 42%\n");
		check(s.find("虛構屬性: 42%") != std::string::npos,
		      "the same shape outside the property block is left alone");
	}

	printf("\n-- GGG's own multi-line entries, registered per line --\n");
	// The dictionary stores these joined, but the game renders them as two lines
	// and a pasted item presents them as two lines, so neither half could match.
	// This one is The Red Nightmare's first line -- it was the last remaining
	// "no translation" gap in the fixtures and it was never a missing entry.
	check_eq("範圍內天賦獲得火焰抗性或全部元素抗性",
	         "Passives granting Fire Resistance or all Elemental Resistances in Radius",
	         "first line of a two-line entry resolves on its own");
	check_eq("同時提供等同其數值 50% 的攻擊傷害格擋率",
	         "also grant Chance to Block Attack Damage at 50% of its value",
	         "second line of the same entry, with its number filled");

	printf("\n-- POB's own '#' placeholder form --\n");
	// POB's crafting UI writes a rollable value as a literal '#'. That string is
	// ALREADY the hashed form, so hashing changed nothing and the pattern lookup
	// used to be skipped entirely -- the affix rows stayed English while the same
	// mod translated fine in the tooltip, where it carries a real number.
	check(fwd_eq("#% increased Armour during Effect", "效果持續時間內，增加 #% 護甲"),
	      "a '#' placeholder line is looked up, not skipped");
	check(fwd_eq("60% increased Armour during Effect", "效果持續時間內，增加 60% 護甲"),
	      "the same mod with a real number is unaffected");

	printf("\n-- reminder text --\n");
	check_eq("（在一小段時間後，他們會造成相同總傷害）", "", "full-width reminder text dropped");
	check_eq("（ 猛攻增加你 20% 攻擊速度、施放速度，和移動速度 ）", "", "flask buff reminder dropped");

	printf("\n-- header values and one-line markers --\n");
	check_eq("範圍: 大", "Radius: Large", "jewel radius value translated");
	check_eq("範圍: 可變的", "Radius: Variable", "variable radius");
	check_eq("破裂之物", "Fractured Item", "Fractured Item (was missing)");
	check_eq("追憶之物", "Synthesised Item", "Synthesised Item (was missing)");
	check_eq("尊師之物", "Elder Item", "Elder Item (was missing)");
	check_eq("塑者之物", "Shaper Item", "Shaper Item (was missing)");
	check_eq("聖戰軍王物品", "Crusader Item", "Crusader Item (was missing)");
	check_eq("分化", "Split", "Split (was missing)");

	printf("\n-- number handling --\n");
	// digits_to_hash absorbs '+' and now '-' alike, so both signs hit one key.
	check_eq("此物品插槽中輔助寶石等級 -2", "-2 to Level of Socketed Support Gems",
	         "negative value matches the same pattern '+2' does");
	check_eq("此物品插槽中技能寶石等級 +2", "+2 to Level of Socketed Skill Gems",
	         "positive value still works");
	// The Chinese side of this one is stored as "{0:+d}"; before normalize_
	// placeholders understood format specs it hashed to garbage and never matched.
	check_eq("效果持續時間內，+10(8-12)% 攻擊傷害格擋率",
	         "+10(8-12)% Chance to Block Attack Damage during Effect",
	         "{0:+d} format spec is the same placeholder as {0}");
	// A '-' BETWEEN two numbers is a separator, not a sign; the socket line would
	// break if that distinction were lost.
	check_eq("插槽: R G-W R", "Sockets: R G-W R", "hyphen between socket groups untouched");
	// Negative rolls put four minus signs in a row. match_range demanded a digit
	// straight after '(', so the range was never collapsed and the whole line
	// failed -- every roll on Melding of the Flesh.
	check_eq("-72(-80--70)% 全部元素抗性",
	         "-72(-80--70)% to all Elemental Resistances",
	         "negative roll range survives");
	check_eq("-4(-6--4)% 全部最大元素抗性",
	         "-4(-6--4)% to all maximum Elemental Resistances",
	         "negative roll range on a maximum-resistance line");
	// The positive form must keep working: the range still has to be one token.
	check_eq("+71(70-84) 最大生命", "+71(70-84) to maximum Life",
	         "positive roll range unaffected");

	printf("\n-- Vestigial items: composed base line, Unscalable marker --\n");
	// The base line is composed from GGG's "Vestigial {0}" template, so the
	// composed form is in no dictionary; the rule translates the base and puts
	// the English prefix back. POB strips "^Vestigial " itself (Item.lua:973).
	check_eq("殘存 匿蹤短靴", "Vestigial Stealth Boots",
	         "Vestigial base-type line composed from its base name");
	// POB parses the ASCII marker itself and sets modLine.unscalable
	// (Item.lua:1059), so the marker is translated, never dropped.
	check_eq("敵人不能偷取你的生命 — 無法使用的值",
	         "Enemies Cannot Leech Life From you - Unscalable Value",
	         "Unscalable marker re-attached through the exact rule");
	check_eq("被榮耀瘋癲影響時，免疫元素異常狀態 — 無法使用的值",
	         "Immune to Elemental Ailments while affected by Glorious Madness - Unscalable Value",
	         "the vestigial implicit itself");
	check_eq("增加 30% 移動速度 — 無法使用的值",
	         "30% increased Movement Speed - Unscalable Value",
	         "Unscalable marker re-attached through the pattern rule");
	// Probed inside a rare item: on a unique the flavour fallback would drop an
	// untouched line and hide what these two guards exist to see.
	{
		std::string s = rev("稀有度: 稀有\n狂喜光澤\n巨型星團珠寶\n--------\n"
		                    "敵人不能偷取你的生命 無法使用的值\n");
		check(s.find("\xe6\x95\xb5\xe4\xba\xba") != std::string::npos /* 敵人 */,
		      "the marker words without the em-dash are prose, not stripped");
	}
	{
		std::string s = rev("稀有度: 稀有\n狂喜光澤\n巨型星團珠寶\n--------\n"
		                    "殘存 這不是任何基底\n");
		check(s.find("\xe6\xae\x98\xe5\xad\x98") != std::string::npos /* 殘存 */,
		      "殘存 before a non-name is not rewritten");
	}

	printf("\n-- flavour text: dropped only when its whole section is untranslated --\n");
	{
		// Alone in its section on a unique -> dropped.
		std::string dropped = rev("稀有度: 傳奇\n噬燼甕\n真銀藥劑\n--------\n新生命有時需要適當的燃燒。\n");
		check(!has_non_ascii(dropped), "unique flavour text section is dropped");

		// The safety property: an untranslated line sharing a section with a
		// translated one is a missing MOD, not flavour, and must stay visible.
		//
		// The specimen is deliberately synthetic. It used to be a real line that
		// had no translation ("範圍內天賦獲得火焰抗性或全部元素抗性"), and when
		// that gap was closed these two assertions started passing vacuously --
		// a test whose premise a fix can remove is not testing what it claims.
		const char* kNoSuchMod = "這不是任何一條詞綴";
		std::string kept = rev(std::string("稀有度: 傳奇\n噬燼甕\n真銀藥劑\n--------\n")
		                       .append(kNoSuchMod).append("\n增加 30% 移動速度\n").c_str());
		check(kept.find(kNoSuchMod) != std::string::npos,
		      "untranslated mod beside a translated one is NOT dropped");

		// Rare items have no flavour text; the same shape must not be touched.
		std::string rare = rev(std::string("稀有度: 稀有\n巨龍 涼靴\n聖騎士長靴\n--------\n")
		                       .append(kNoSuchMod).append("\n").c_str());
		check(rare.find(kNoSuchMod) != std::string::npos,
		      "rare items are never subject to the flavour-text rule");
	}

	printf("\n-- separators --\n");
	{
		// Not testable on its own: translation_reverse_text deliberately returns
		// nothing for pure-ASCII input, so the separator has to ride along inside
		// a real item. 3.29 sizes the dash row to the section width; POB compares
		// against exactly eight (Classes/Item.lua:424).
		std::string s = rev("稀有度: 稀有\n巨龍 涼靴\n聖騎士長靴\n------------------------\n物品等級: 84\n");
		std::vector<std::string> ls = split_lines(s);
		bool has8 = false, hasLong = false;
		for (const std::string& l : ls) {
			if (l == "--------") has8 = true;
			if (l.size() > 8 && l.find_first_not_of('-') == std::string::npos) hasLong = true;
		}
		check(has8 && !hasLong, "variable-width separator normalised to 8 dashes");
	}

	printf("\npaste selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
