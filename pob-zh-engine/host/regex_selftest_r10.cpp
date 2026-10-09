// --regex-selftest, R10 part: merging pages of one item group only, the rarity /
// corruption conditions de-duplicated (intersection, ConditionClash), and the
// shortened condition fragments that match the game's copied item text
// ("稀有度: 稀有", "怪群大小: +13% (augmented)", "換界石（階級 2）").
//
// Spec table (G1-G3, D1-D4, F1-F9, S1-S3, E1) from the coordinator, 2026-10-09.
// Real item text: tests/regex_samples/poe2_items_zh.txt and poe1_maps_zh.txt
// (copied from the game by the user; items separated by a blank line). The game's
// search bar matches each term line by line, case-insensitively; "!term" holds
// when no line matches.
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_frag.h"
#include "regex_gen.h"
#include "regex_match.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace RegexAlgo;
using RegexGen::Mode;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;
void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

// ---- the catalogue, as the panel builds it -------------------------------------

struct Game {
	std::string id;
	std::vector<AlgoPage> algo;
	std::vector<PageRef> pages;
	const AlgoPage* Algo(const std::string& pid) const
	{
		for (const AlgoPage& p : algo)
			if (p.id == pid) return &p;
		return nullptr;
	}
	const RegexPageDef* Corpus(const std::string& pid) const
	{
		for (const PageRef& r : pages)
			if (r.corpus && r.corpus->id == pid) return r.corpus;
		return nullptr;
	}
};

int IdxOf(const AlgoPage& p, const std::string& id)
{
	for (int i = 0; i < (int)p.entries.size(); i++)
		if (p.entries[i].def.id == id) return i;
	return -1;
}
int IdxOf(const RegexPageDef& p, const std::string& id)
{
	for (int i = 0; i < (int)p.entries.size(); i++)
		if (p.entries[i].id == id) return i;
	return -1;
}
// The rarity | corruption row of a page (InputKind::Rarity); -1 if none.
int RarityIdx(const AlgoPage& p)
{
	for (int i = 0; i < (int)p.entries.size(); i++)
		if (p.entries[i].input.kind == InputKind::Rarity) return i;
	return -1;
}

AlgoValue Choice(std::vector<std::string> rarity, Corruption c = Corruption::None)
{
	RarityChoice rc;
	rc.rarity = std::move(rarity);
	rc.corruption = c;
	AlgoValue v;
	v.choice = EncodeRarityChoice(rc);
	v.hasChoice = true;
	return v;
}
AlgoValue Min(double n)
{
	AlgoValue v;
	v.min = n;
	return v;
}

CombineSel CorpusSel(const RegexPageDef* p, std::vector<int> picks, Lang lang)
{
	CombineSel s;
	s.page.corpus = p;
	s.corpus = nullptr;   // Combine builds it: the fake pages below are short-lived copies, no pointer-keyed cache
	(void)lang;
	s.picks = std::move(picks);
	return s;
}
CombineSel AlgoSel(const AlgoPage* p, std::vector<int> picks, const ValueMap* v)
{
	CombineSel s;
	s.page.algo = p;
	s.picks = std::move(picks);
	s.values = v;
	return s;
}

// The query split into its terms, quotes removed ("a b" c -> [a b, c]).
std::vector<std::string> Terms(const std::string& q)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i < q.size()) {
		if (q[i] == ' ') { i++; continue; }
		if (q[i] == '"') {
			const size_t e = q.find('"', i + 1);
			out.push_back(q.substr(i + 1, (e == std::string::npos ? q.size() : e) - i - 1));
			i = e == std::string::npos ? q.size() : e + 1;
		} else {
			const size_t e = q.find(' ', i);
			out.push_back(q.substr(i, (e == std::string::npos ? q.size() : e) - i));
			i = e == std::string::npos ? q.size() : e;
		}
	}
	return out;
}
std::string JoinTerms(const std::vector<std::string>& t)
{
	std::string s;
	for (const std::string& x : t) s += (s.empty() ? "[" : ", [") + x + "]";
	return s.empty() ? "(none)" : s;
}
bool Has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Does pattern `f` hit `text` (one line)? A syntax error counts as no hit and is reported.
bool Hits(const std::string& f, const std::string& text)
{
	std::string err;
	const std::optional<Rx> rx = RxCompile(f, &err);
	if (!rx) {
		line("    ! pattern does not compile: " + f + " -- " + err);
		return false;
	}
	return RxSearch(*rx, text);
}
// A positive term holds for an item when some line matches; "!x" when none does.
bool HoldsFor(const std::string& term, const std::vector<std::string>& item)
{
	const bool neg = !term.empty() && term[0] == '!';
	const std::string f = neg ? term.substr(1) : term;
	bool any = false;
	for (const std::string& l : item) any |= Hits(f, l);
	return neg ? !any : any;
}

// "稀有度: 稀有" as the game prints it, from the game's clientstrings (PoE2 prints
// normal as "中"): RarityLabelKeys() = label, normal, magic, rare, unique, corrupted.
const RegexLabels* g_rarityLabels = nullptr;
std::string RarityLine(Lang lang, int i)
{
	static const char* const zh[] = {u8"普通", u8"魔法", u8"稀有", u8"傳奇"};
	static const char* const en[] = {"Normal", "Magic", "Rare", "Unique"};
	const std::vector<std::string>& keys = RarityLabelKeys();
	const std::string* label = g_rarityLabels && keys.size() > 4 ? g_rarityLabels->Find(lang == Lang::Zh, keys[0]) : nullptr;
	const std::string* word = g_rarityLabels && keys.size() > 4 ? g_rarityLabels->Find(lang == Lang::Zh, keys[1 + i]) : nullptr;
	return (label ? RegexFrag::LabelBase(*label) : std::string(lang == Lang::Zh ? u8"稀有度" : "Rarity")) + ": " +
	       (word ? *word : std::string(lang == Lang::Zh ? zh[i] : en[i]));
}
// A rarity term: positive, and it matches one of the four "稀有度: X" lines.
bool IsRarityTerm(const std::string& t, Lang lang)
{
	if (t.empty() || t[0] == '!') return false;
	for (int i = 0; i < 4; i++)
		if (Hits(t, RarityLine(lang, i))) return true;
	return false;
}

int CountKind(const CombineResult& r, ConflictKind k)
{
	int n = 0;
	for (const Conflict& c : r.conflicts) n += c.kind == k;
	return n;
}

// The condition terms Combine prints for one section pick, the host page's corpus
// taking part (its picks empty), as the panel does for the single-page output.
std::vector<std::string> SectionTerms(const RegexPageDef* host, const AlgoPage* sec, int idx, const AlgoValue& v, Lang lang)
{
	ValueMap vm;
	vm[sec->entries[idx].def.id] = v;
	const CombineResult r = Combine(lang, Mode::Any, {CorpusSel(host, {}, lang), AlgoSel(sec, {idx}, &vm)});
	return Terms(r.query);
}

// ---- real item text (tests/regex_samples) ----------------------------------------

std::wstring Widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

bool ReadAll(const std::wstring& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
		out.erase(0, 3);
	return true;
}

// tests/regex_samples/<name>: next to this source file (the build tree knows where
// the repo is), else walking up from the exe directory.
bool ReadSample(const std::wstring& exeDir, const std::wstring& name, std::string& out, std::wstring* where)
{
	std::vector<std::wstring> tries;
	{
		std::wstring src = Widen(__FILE__);
		const size_t s = src.find_last_of(L"\\/");
		if (s != std::wstring::npos) tries.push_back(src.substr(0, s) + L"\\..\\tests\\regex_samples\\" + name);
	}
	std::wstring d = exeDir;
	for (int up = 0; up < 5 && !d.empty(); up++) {
		while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
		tries.push_back(d + L"\\tests\\regex_samples\\" + name);
		tries.push_back(d + L"\\pob-zh-engine\\tests\\regex_samples\\" + name);
		const size_t s = d.find_last_of(L"\\/");
		if (s == std::wstring::npos) break;
		d = d.substr(0, s);
	}
	for (const std::wstring& p : tries)
		if (ReadAll(p, out)) {
			if (where) *where = p;
			return true;
		}
	return false;
}

// Items separated by blank lines; "\r" dropped.
std::vector<std::vector<std::string>> SplitItems(const std::string& text)
{
	std::vector<std::vector<std::string>> items(1);
	std::istringstream in(text);
	std::string l;
	while (std::getline(in, l)) {
		if (!l.empty() && l.back() == '\r') l.pop_back();
		if (l.empty()) {
			if (!items.back().empty()) items.emplace_back();
			continue;
		}
		items.back().push_back(l);
	}
	if (items.back().empty()) items.pop_back();
	return items;
}

bool ItemHasSub(const std::vector<std::string>& item, const std::string& sub)
{
	for (const std::string& l : item)
		if (l.find(sub) != std::string::npos) return true;
	return false;
}

// The waystone the user copied (spec text; also the first item of poe2_items_zh.txt).
const std::vector<std::string> kWaystone = {
	u8"物品種類: 換界石",
	u8"稀有度: 稀有",
	u8"幽暗方向",
	u8"換界石（階級 2）",
	u8"--------",
	u8"可用的復活數: 2 (augmented)",
	u8"怪群大小: +13% (augmented)",
	u8"怪物效能: +28% (augmented)",
	u8"換界石掉落機率: +50% (augmented)",
	u8"--------",
	u8"物品等級: 68",
	u8"--------",
	u8"{ 前綴 \"穿孔的\"(階層：1) }",
	u8"怪物有6(5-10)%機率在擊中時引起流血",
	u8"{ 前綴 \"精確的\"(階層：1) }",
	u8"怪物增加20(10-20)%命中值",
	u8"{ 後綴 \"烈火之\"(階層：1) }",
	u8"區域中有多個點燃地面",
	u8"{ 後綴 \"閃避之\"(階層：1) }",
	u8"怪物為閃避的",
	u8"--------",
	u8"可用於地圖裝置以進入地圖。每個換界石只能被使用一次。",
};

// ---- G: merging only within one item group --------------------------------------

void GroupTests(const std::map<std::string, Game>& games)
{
	line(u8"[R10-G] 合併只限同一物品組");
	const Game& P2 = games.at("poe2");

	// G3
	{
		const std::set<std::string> equip = {"vendor_bases", "vendor_items", "vendor_items_poe2", "item_mod_values",
		                                     "item_mod_values_poe2", "flask_mods", "flask_charm_mods", "gem_names"};
		int missing = 0, wrongEquip = 0, wrongSection = 0, wrongOwn = 0;
		std::string first;
		for (const auto& kv : games) {
			std::vector<std::string> ids;
			for (const PageRef& r : kv.second.pages) ids.push_back(r.Id());
			ids.push_back(kv.first == "poe2" ? "item_mod_values_poe2" : "item_mod_values");   // built on demand by the panel
			for (const std::string& id : ids) {
				const std::string g = ItemGroupOf(id);
				const std::string host = SectionHostOf(id);
				if (g.empty()) { missing++; if (first.empty()) first = id + u8" 沒有組"; continue; }
				if (!host.empty()) {
					if (g != ItemGroupOf(host)) { wrongSection++; if (first.empty()) first = id + " -> " + g + u8"，宿主 " + host + " -> " + ItemGroupOf(host); }
				} else if (equip.count(id)) {
					if (g != "equipment") { wrongEquip++; if (first.empty()) first = id + " -> " + g; }
				} else if (g != id) {
					wrongOwn++;
					if (first.empty()) first = id + " -> " + g;
				}
			}
		}
		check(missing == 0, u8"G3 物品組對照完整：兩遊戲每頁都有組（缺 " + Num(missing) + u8"）" + (first.empty() ? "" : u8"，首例 " + first));
		check(wrongEquip == 0 && ItemGroupOf("vendor_bases") == "equipment" && ItemGroupOf("flask_charm_mods") == "equipment" &&
		      ItemGroupOf("gem_names") == "equipment" &&
		      ItemGroupOf("item_mod_values_poe2") == "equipment" && ItemGroupOf("vendor_items") == "equipment",
		      u8"G3 物品組對照完整：裝備組成員（商店基底 / 商店物品 / 物品詞綴數值 / 藥劑 / 護符）都是 equipment");
		check(wrongSection == 0 && ItemGroupOf("waystone_numeric") == ItemGroupOf("waystone_mods") &&
		      !ItemGroupOf("waystone_numeric").empty() && ItemGroupOf("tablet_mods_cond") == "tablet_mods" &&
		      ItemGroupOf("item_mod_values_poe2_cond") == "equipment" && ItemGroupOf("map_numeric") == "map_mods",
		      u8"G3 物品組對照完整：section 歸宿主組");
		check(wrongOwn == 0 && ItemGroupOf("tablet_mods") == "tablet_mods" && ItemGroupOf("waystone_mods") == "waystone_mods" &&
		      ItemGroupOf("tablet_mods") != ItemGroupOf("waystone_mods"),
		      u8"G3 物品組對照完整：其餘頁自成一組（組名 = 頁 id），碑牌與換界石不同組");
	}

	// G1 / E1 share the screenshot setup.
	const RegexPageDef* tablet = P2.Corpus("tablet_mods");
	const RegexPageDef* waystone = P2.Corpus("waystone_mods");
	const AlgoPage* tabletCond = P2.Algo("tablet_mods_cond");
	const AlgoPage* wn = P2.Algo("waystone_numeric");
	if (!tablet || !waystone || !tabletCond || !wn) {
		check(false, u8"G1/E1：poe2 tablet_mods / waystone_mods / tablet_mods_cond / waystone_numeric 都在");
		return;
	}
	const int verisium = IdxOf(*tablet, "TowerExpeditionIncreasedVerisium");
	const int tcR = RarityIdx(*tabletCond);
	const int wTier = IdxOf(*wn, "tier"), wR = IdxOf(*wn, kRarityEntryId);
	check(verisium >= 0 && tcR >= 0 && wTier >= 0 && wR >= 0,
	      u8"G1/E1 前提：碑牌有「增加#%維里西姆遺物怪物掉落的維里西姆」、碑牌條件區與換界石數值區有稀有度列、換界石有階級列");
	if (verisium < 0 || tcR < 0 || wTier < 0 || wR < 0) return;

	ValueMap tv, wv;
	tv[tabletCond->entries[tcR].def.id] = Choice({"rare"});
	wv["tier"] = Min(15);
	wv[kRarityEntryId] = Choice({"magic", "rare"}, Corruption::Uncorrupted);
	// The panel's combine order: each host followed by its section.
	struct Picked { std::string id; CombineSel sel; };
	const std::vector<Picked> all = {
		{"waystone_mods", CorpusSel(waystone, {}, Lang::Zh)},
		{"waystone_numeric", AlgoSel(wn, {wTier, wR}, &wv)},
		{"tablet_mods", CorpusSel(tablet, {verisium}, Lang::Zh)},
		{"tablet_mods_cond", AlgoSel(tabletCond, {tcR}, &tv)},
	};
	// Pages with ticks (waystone_mods itself has none; its section does).
	const std::vector<std::string> picked = {"waystone_numeric", "tablet_mods", "tablet_mods_cond"};
	const MergePlan plan = PlanMerge("tablet_mods", picked);
	std::vector<CombineSel> sels;
	for (const Picked& p : all)
		if (Has(plan.merged, p.id)) sels.push_back(p.sel);
	const CombineResult r = Combine(Lang::Zh, Mode::Any, sels);
	std::set<std::string> perIds;
	for (const PageContribution& c : r.perPage)
		if (c.picked > 0) perIds.insert(c.id);
	const std::vector<std::string> terms = Terms(r.query);

	// G1
	check(r.query.find(u8"階級") == std::string::npos,
	      u8"G1 碑牌頁合併不含換界石條件：輸出不含「階級」 -- " + r.query);
	check(perIds == std::set<std::string>{"tablet_mods", "tablet_mods_cond"} && plan.merged == std::vector<std::string>{"tablet_mods", "tablet_mods_cond"},
	      u8"G1 碑牌頁合併不含換界石條件：只併入碑牌與其條件區（併入 " + JoinTerms(plan.merged) + "）");
	check(plan.skippedPages == 1, u8"G1 碑牌頁合併不含換界石條件：回報 1 頁未併入（得 " + Num(plan.skippedPages) + "）");

	// E1
	check(terms == std::vector<std::string>{u8"%維", u8"度: 稀"},
	      u8"E1 截圖案例端到端：以碑牌為目前頁合併 = \"%維\" \"度: 稀\" -- 得 " + r.query);

	// G2
	{
		const MergePlan p = PlanMerge("vendor_bases", {"vendor_bases", "vendor_bases_cond", "item_mod_values_poe2"});
		check(p.merged == std::vector<std::string>{"vendor_bases", "vendor_bases_cond", "item_mod_values_poe2"} && p.skippedPages == 0,
		      u8"G2 裝備組可合併：商店基底 + 物品詞綴數值頁都併入");
		const MergePlan q = PlanMerge("item_mod_values_poe2", {"vendor_bases", "item_mod_values_poe2", "tablet_mods"});
		check(q.merged == std::vector<std::string>{"vendor_bases", "item_mod_values_poe2"} && q.skippedPages == 1,
		      u8"G2 裝備組可合併：從物品詞綴數值頁看，商店基底併入、碑牌不併入（1 頁）");
	}

	// G5 (2026-10-09, user-approved spec change): gem names + the vendor page's gem level merge.
	for (const auto& kv : games) {
		const Game& G = kv.second;
		const std::string vid = kv.first == "poe2" ? "vendor_items_poe2" : "vendor_items";
		const RegexPageDef* gems = G.Corpus("gem_names");
		const AlgoPage* vendor = G.Algo(vid);
		const int gl = vendor ? IdxOf(*vendor, "gem_level") : -1;
		if (!gems || gems->entries.empty() || gl < 0) {
			check(false, u8"G5 前提（" + kv.first + u8"）：gem_names 有寶石、" + vid + u8" 有寶石等級列");
			continue;
		}
		const MergePlan p = PlanMerge("gem_names", {"gem_names", vid});
		ValueMap vv;
		vv["gem_level"] = Min(3);
		std::vector<CombineSel> sels;
		for (const CombineSel& s : {CorpusSel(gems, {0}, Lang::Zh), AlgoSel(vendor, {gl}, &vv)}) {
			const std::string id = s.page.Id();
			if (Has(p.merged, id)) sels.push_back(s);
		}
		const CombineResult r = Combine(Lang::Zh, Mode::Any, sels);
		int contributed = 0;
		for (const PageContribution& c : r.perPage) contributed += c.picked > 0;
		check(p.merged == std::vector<std::string>{"gem_names", vid} && p.skippedPages == 0 && contributed == 2,
		      u8"G5 寶石名稱與商店物品條件可合併（" + kv.first + u8"）：以 gem_names 為目前頁，寶石 + 寶石等級 ≥3 兩頁都併入 -- " + r.query);
		const MergePlan q = PlanMerge("tablet_mods", {"gem_names", "tablet_mods"});
		check(q.merged == std::vector<std::string>{"tablet_mods"} && q.skippedPages == 1,
		      u8"G5 寶石名稱與商店物品條件可合併（" + kv.first + u8"）：反向，以碑牌為目前頁時 gem_names 不併入（1 頁）");
	}
}

// ---- D: condition de-duplication -----------------------------------------------

void DedupTests(const std::map<std::string, Game>& games)
{
	line(u8"[R10-D] 合併時稀有度 / 汙染條件取交集、去重");
	const Game& P2 = games.at("poe2");
	const AlgoPage* a = P2.Algo("vendor_bases_cond");
	const AlgoPage* b = P2.Algo("item_mod_values_poe2_cond");
	if (!a || !b || RarityIdx(*a) < 0 || RarityIdx(*b) < 0) {
		check(false, u8"D：poe2 vendor_bases_cond / item_mod_values_poe2_cond 都有稀有度列");
		return;
	}
	const int ia = RarityIdx(*a), ib = RarityIdx(*b);
	auto run = [&](const AlgoValue& va, const AlgoValue& vb, Lang lang) {
		ValueMap ma, mb;
		ma[a->entries[ia].def.id] = va;
		mb[b->entries[ib].def.id] = vb;
		return Combine(lang, Mode::Any, {AlgoSel(a, {ia}, &ma), AlgoSel(b, {ib}, &mb)});
	};

	// D1
	for (Lang lang : {Lang::Zh, Lang::En}) {
		const CombineResult r = run(Choice({"magic", "rare"}), Choice({"rare"}), lang);
		std::vector<std::string> rar;
		for (const std::string& t : Terms(r.query))
			if (IsRarityTerm(t, lang)) rar.push_back(t);
		const bool onlyRare = rar.size() == 1 && Hits(rar[0], RarityLine(lang, 2)) && !Hits(rar[0], RarityLine(lang, 1)) &&
		                      !Hits(rar[0], RarityLine(lang, 0)) && !Hits(rar[0], RarityLine(lang, 3));
		check(onlyRare && CountKind(r, ConflictKind::ConditionClash) == 0 && r.ok,
		      std::string(u8"D1 稀有度取交集（") + (lang == Lang::Zh ? "zh" : "en") + u8"）：(魔法|稀有) + 稀有 -> 只剩一個稀有度 term、值 = 稀有 -- 稀有度 term " +
		          JoinTerms(rar) + u8"，query " + r.query);
	}
	// D2
	{
		const CombineResult r = run(Choice({"magic"}), Choice({"rare"}), Lang::Zh);
		check(CountKind(r, ConflictKind::ConditionClash) >= 1 && !r.ok,
		      u8"D2 稀有度交集為空：魔法 vs 稀有 -> ConditionClash、結果不 ok -- query " + r.query);
	}
	// D3
	{
		const CombineResult r = run(Choice({}, Corruption::Uncorrupted), Choice({}, Corruption::Corrupted), Lang::Zh);
		check(CountKind(r, ConflictKind::ConditionClash) >= 1 && !r.ok,
		      u8"D3 汙染衝突：未汙染 vs 已汙染 -> ConditionClash、結果不 ok -- query " + r.query);
	}
	// D4
	for (Lang lang : {Lang::Zh, Lang::En}) {
		const CombineResult r = run(Choice({}, Corruption::Uncorrupted), Choice({}, Corruption::Uncorrupted), lang);
		const std::vector<std::string> t = Terms(r.query);
		std::set<std::string> uniq(t.begin(), t.end());
		check(t.size() == 1 && uniq.size() == 1 && t[0][0] == '!' && CountKind(r, ConflictKind::ConditionClash) == 0,
		      std::string(u8"D4 相同 term 去重（") + (lang == Lang::Zh ? "zh" : "en") + u8"）：兩頁同為未汙染 -> 只出一次 -- 得 " + JoinTerms(t));
	}
}

// ---- F / S: shortened condition fragments ----------------------------------------

void FragmentTests(const std::map<std::string, Game>& games, const std::wstring& exeDir)
{
	line(u8"[R10-F] 條件片段縮短（PoE2 / PoE1 皆「標籤: 值」半形冒號 + 一空白）");
	const Game& P1 = games.at("poe1");
	const Game& P2 = games.at("poe2");
	const RegexPageDef* waystone = P2.Corpus("waystone_mods");
	const AlgoPage* wn = P2.Algo("waystone_numeric");
	const RegexPageDef* maps = P1.Corpus("map_mods");
	const AlgoPage* mn = P1.Algo("map_numeric");
	if (!waystone || !wn || !maps || !mn) {
		check(false, u8"F：waystone_mods / waystone_numeric / map_mods / map_numeric 都在");
		return;
	}
	const int wR = IdxOf(*wn, kRarityEntryId), wTier = IdxOf(*wn, "tier"), wPack = IdxOf(*wn, "pack");
	const int mR = IdxOf(*mn, kRarityEntryId), mTier = IdxOf(*mn, "tier"), mQty = IdxOf(*mn, "quantity");
	if (wR < 0 || wTier < 0 || wPack < 0 || mR < 0 || mTier < 0 || mQty < 0) {
		check(false, u8"F：數值區有 稀有度 / 階級 / 怪群大小 / 物品數量 列");
		return;
	}

	// F1
	{
		const std::vector<std::string> zh = SectionTerms(waystone, wn, wR, Choice({"rare"}), Lang::Zh);
		const std::vector<std::string> en = SectionTerms(waystone, wn, wR, Choice({"rare"}), Lang::En);
		check(zh == std::vector<std::string>{u8"度: 稀"}, u8"F1 PoE2 稀有度單選縮短（zh 稀有）-> 度: 稀 -- 得 " + JoinTerms(zh));
		check(en == std::vector<std::string>{"y: r"}, u8"F1 PoE2 稀有度單選縮短（en Rare）-> y: r -- 得 " + JoinTerms(en));
	}
	// F2
	{
		const std::vector<std::string> zh = SectionTerms(waystone, wn, wR, Choice({"magic", "rare"}), Lang::Zh);
		const std::vector<std::string> en = SectionTerms(waystone, wn, wR, Choice({"magic", "rare"}), Lang::En);
		check(zh == std::vector<std::string>{u8"度: [魔稀]"}, u8"F2 PoE2 稀有度多選（魔法+稀有）-> 度: [魔稀] -- 得 " + JoinTerms(zh));
		check(en == std::vector<std::string>{"y: [mr]"}, u8"F2 PoE2 稀有度多選（Magic+Rare）-> y: [mr] -- 得 " + JoinTerms(en));
	}
	// F3
	{
		check(HoldsFor(u8"度: 稀", kWaystone) && !HoldsFor(u8"度: [魔中]", kWaystone),
		      u8"F3 稀有度片段比對真實物品文字：度: 稀 命中換界石、度: [魔中] 不命中");
		const std::vector<std::string> rare = SectionTerms(waystone, wn, wR, Choice({"rare"}), Lang::Zh);
		const std::vector<std::string> mnorm = SectionTerms(waystone, wn, wR, Choice({"normal", "magic"}), Lang::Zh);
		check(rare.size() == 1 && HoldsFor(rare[0], kWaystone) && mnorm.size() == 1 && !HoldsFor(mnorm[0], kWaystone),
		      u8"F3 稀有度片段比對真實物品文字：產生的「稀有」片段命中、「普通+魔法」片段不命中 -- " + JoinTerms(rare) + " / " + JoinTerms(mnorm));
	}
	// F4
	{
		RegexPageDef fake = *waystone;
		RegexEntryDef e;
		e.id = "r10_fake_rarity_lookalike";
		e.zh = {u8"測試強度: 稀少的#"};
		e.en = {"Test Intensity: rx #"};
		fake.entries.push_back(e);
		const std::vector<std::string> zh = SectionTerms(&fake, wn, wR, Choice({"rare"}), Lang::Zh);
		const std::vector<std::string> en = SectionTerms(&fake, wn, wR, Choice({"rare"}), Lang::En);
		check(zh.size() == 1 && zh[0].find(u8"稀有度") != std::string::npos && zh[0] != u8"度: 稀" && !Hits(zh[0], u8"測試強度: 稀少的5") &&
		          Hits(zh[0], u8"稀有度: 稀有"),
		      u8"F4 稀有度防誤中退回（zh）：語料有含「度: 稀」的行 -> 退回含「稀有度」的完整寫法 -- 得 " + JoinTerms(zh));
		check(en.size() == 1 && en[0].find("Rarity") != std::string::npos && en[0] != "y: r" && !Hits(en[0], "Test Intensity: rx 5") &&
		          Hits(en[0], "Rarity: Rare"),
		      u8"F4 稀有度防誤中退回（en）：語料有含「y: r」的行 -> 退回含「Rarity」的完整寫法 -- 得 " + JoinTerms(en));
	}
	// F5
	for (Lang lang : {Lang::Zh, Lang::En}) {
		const std::string full = lang == Lang::Zh ? std::string(u8"已汙染") : std::string("Corrupted");
		const std::string want = lang == Lang::Zh ? std::string(u8"已汙") : std::string("corr");
		const std::vector<std::string> c = SectionTerms(waystone, wn, wR, Choice({}, Corruption::Corrupted), lang);
		const std::vector<std::string> u = SectionTerms(waystone, wn, wR, Choice({}, Corruption::Uncorrupted), lang);
		const std::string tag = lang == Lang::Zh ? "zh" : "en";
		std::string lowFull = full;
		std::transform(lowFull.begin(), lowFull.end(), lowFull.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
		const bool prefix = c.size() == 1 && !c[0].empty() && c[0].size() < full.size() &&
		                    (lang == Lang::Zh ? full.compare(0, c[0].size(), c[0]) == 0 : lowFull.compare(0, c[0].size(), c[0]) == 0);
		check(prefix && c[0] == want, u8"F5 汙染縮短（" + tag + u8"）：已汙染 -> 最短安全前綴 " + want + u8" -- 得 " + JoinTerms(c));
		check(c.size() == 1 && u.size() == 1 && u[0] == "!" + c[0],
		      u8"F5 汙染縮短（" + tag + u8"）：未汙染 = ! + 同一片段 -- 得 " + JoinTerms(u));
		// The fragment hits no other line of the page's corpus.
		int other = 0;
		std::string firstOther;
		if (c.size() == 1)
			for (const RegexEntryDef& e : waystone->entries)
				for (const std::string& l : lang == Lang::Zh ? e.zh : e.en)
					if (l != full && Hits(c[0], l)) { if (!other++) firstOther = l; }
		check(c.size() == 1 && other == 0, u8"F5 汙染縮短（" + tag + u8"）：片段不中本頁其他語料行" + (other ? u8" -- 中 " + firstOther : std::string()));
	}
	{
		const std::vector<std::string> c = SectionTerms(waystone, wn, wR, Choice({}, Corruption::Corrupted), Lang::Zh);
		std::vector<std::string> corrupted = kWaystone;
		corrupted.push_back(u8"已汙染");
		check(c == std::vector<std::string>{u8"已汙"} && !HoldsFor(c[0], kWaystone) && HoldsFor(c[0], corrupted) &&
		          HoldsFor("!" + c[0], kWaystone) && !HoldsFor("!" + c[0], corrupted),
		      u8"F5 汙染縮短：換界石全文（未汙染）不被 已汙 命中，加一行「已汙染」後命中");
	}
	// F6
	{
		RegexPageDef fake = *waystone;
		RegexEntryDef e;
		e.id = "r10_fake_corrupted_prefix";
		e.zh = {u8"已汙染的地圖掉落機率 #%"};
		e.en = {"Corrupted Maps drop chance #%"};
		fake.entries.push_back(e);
		const std::vector<std::string> zh = SectionTerms(&fake, wn, wR, Choice({}, Corruption::Corrupted), Lang::Zh);
		const std::vector<std::string> zhU = SectionTerms(&fake, wn, wR, Choice({}, Corruption::Uncorrupted), Lang::Zh);
		check(zh == std::vector<std::string>{u8"^已汙染$"} && zhU == std::vector<std::string>{u8"!^已汙染$"},
		      u8"F6 汙染退回：語料含「已汙染的…」行 -> ^已汙染$ / !^已汙染$ -- 得 " + JoinTerms(zh) + " / " + JoinTerms(zhU));
		// The real PoE1 map page has "已汙染物品機率": no prefix of 已汙染 is safe there either.
		const std::vector<std::string> real = SectionTerms(maps, mn, mR, Choice({}, Corruption::Corrupted), Lang::Zh);
		check(real == std::vector<std::string>{u8"^已汙染$"},
		      u8"F6 汙染退回：PoE1 地圖頁（語料有「已汙染物品機率」）-> ^已汙染$ -- 得 " + JoinTerms(real));
	}
	// F7
	{
		const AlgoEntry& t = wn->entries[wTier];
		const std::optional<std::string> f = t.fragment(Min(15), Lang::Zh);
		check(t.input.hi == 16 && f && *f == u8"階級 *1[56]）", u8"F7 階級帶上限：PoE2 ≥15（hi 16）-> 階級 *1[56]） -- 得 " + (f ? *f : std::string("(null)")));
		check(f && Hits(*f, u8"換界石（階級 16）") && Hits(*f, u8"換界石（階級 15）") && !Hits(*f, u8"換界石（階級 2）") && !Hits(*f, u8"換界石（階級 1）"),
		      u8"F7 階級帶上限：比對 換界石（階級 16）命中、（階級 2）不命中");
	}
	// F8
	{
		const std::optional<std::string> f = wn->entries[wPack].fragment(Min(10), Lang::Zh);
		const std::string s = f ? *f : std::string("(null)");
		check(f && Hits(*f, u8"怪群大小: +13% (augmented)") && !Hits(*f, u8"怪群大小: +9% (augmented)"),
		      u8"F8 PoE2 數值行縮短：怪群大小 ≥10% 命中「怪群大小: +13% (augmented)」、不中 +9% -- " + s);
		check(f && f->find(u8"[:：]") == std::string::npos && f->rfind(u8"怪群大小: ", 0) == 0 && f->find(" *%") == std::string::npos,
		      u8"F8 PoE2 數值行縮短：片段為「怪群大小: \\+?…%」（不含 [:：]、% 前無 \" *\"）-- " + s);
	}
	// F9 (updated 2026-10-09: PoE1 copies "稀有度: 稀有" / "物品數量: +68% (augmented)" too)
	{
		const std::vector<std::string> zh = SectionTerms(maps, mn, mR, Choice({"rare"}), Lang::Zh);
		check(zh == std::vector<std::string>{u8"度: 稀"}, u8"F9 PoE1 稀有度也用新格式：zh 稀有 -> 度: 稀 -- 得 " + JoinTerms(zh));
		const std::optional<std::string> q = mn->entries[mQty].fragment(Min(60), Lang::Zh);
		check(q && q->find(u8"[:：]") == std::string::npos && q->rfind(u8"物品數量: ", 0) == 0,
		      u8"F9 PoE1 數值行也用新格式：物品數量片段以「物品數量: 」開頭、不含 [:：] -- " + (q ? *q : std::string("(null)")));
		const std::optional<std::string> t = mn->entries[mTier].fragment(Min(16), Lang::Zh);
		check(mn->entries[mTier].input.hi == 17 && t && *t == u8"階級 *1[67]）",
		      u8"F9 PoE1 階級帶上限：≥16（hi 17）-> 階級 *1[67]） -- 得 " + (t ? *t : std::string("(null)")));
	}

	// ---- S: the user's real items ----
	line(u8"[R10-S] 真實物品全文（tests/regex_samples）");
	std::string t1, t2;
	std::wstring w1, w2;
	const bool ok1 = ReadSample(exeDir, L"poe2_items_zh.txt", t1, &w1);
	const bool ok2 = ReadSample(exeDir, L"poe1_maps_zh.txt", t2, &w2);
	check(ok1 && ok2, u8"S 樣本檔讀得到：poe2_items_zh.txt / poe1_maps_zh.txt");
	if (!ok1 || !ok2) return;
	const auto items2 = SplitItems(t1);
	const auto items1 = SplitItems(t2);
	check(items2.size() == 2 && items1.size() == 9, u8"S 樣本檔物品數：PoE2 2 件、PoE1 9 件（得 " + Num((long long)items2.size()) + " / " +
	                                                    Num((long long)items1.size()) + "）");
	check(items2.size() >= 1 && items2[0] == kWaystone, u8"S PoE2 樣本第一件 = 規格附的換界石全文");

	// S1
	{
		struct Src { const char* name; const std::vector<std::vector<std::string>>* items; const RegexPageDef* host; const AlgoPage* sec; int r; };
		const Src srcs[] = {{"poe2", &items2, waystone, wn, wR}, {"poe1", &items1, maps, mn, mR}};
		for (const Src& s : srcs) {
			const std::vector<std::string> rare = SectionTerms(s.host, s.sec, s.r, Choice({"rare"}), Lang::Zh);
			const std::vector<std::string> mnn = SectionTerms(s.host, s.sec, s.r, Choice({"normal", "magic"}), Lang::Zh);
			const std::vector<std::string> unc = SectionTerms(s.host, s.sec, s.r, Choice({}, Corruption::Uncorrupted), Lang::Zh);
			int bad = 0;
			std::string why;
			for (size_t i = 0; i < s.items->size(); i++) {
				const auto& it = (*s.items)[i];
				const bool a = rare.size() == 1 && HoldsFor(rare[0], it);
				const bool b = mnn.size() == 1 && !HoldsFor(mnn[0], it);
				const bool c = unc.size() == 1 && HoldsFor(unc[0], it);
				const bool d = HoldsFor(u8"度: 稀", it) && !HoldsFor(u8"度: [魔中]", it) && HoldsFor(u8"!已汙", it);
				if (!(a && b && c && d)) {
					if (!bad++) why = u8"第 " + Num((long long)i + 1) + u8" 件：稀有 " + (a ? "ok" : "X") + u8"、普通+魔法不中 " + (b ? "ok" : "X") +
					                  u8"、未汙染 " + (c ? "ok" : "X") + u8"、字面片段 " + (d ? "ok" : "X");
				}
			}
			check(bad == 0, std::string(u8"S1 ") + s.name + u8" 每件物品：稀有片段命中、普通+魔法片段不中、未汙染成立（" + JoinTerms(rare) + " / " +
			                    JoinTerms(mnn) + " / " + JoinTerms(unc) + u8"）" + (bad ? " -- " + why : std::string()));
		}
	}
	// S2
	{
		const std::optional<std::string> f = mn->entries[mTier].fragment(Min(16), Lang::Zh);
		int bad = 0, with16 = 0, low = 0;
		std::string why;
		for (size_t i = 0; i < items1.size(); i++) {
			const bool is16 = ItemHasSub(items1[i], u8"（階級 16）") || ItemHasSub(items1[i], u8"（階級 17）");
			const bool isLow = ItemHasSub(items1[i], u8"（階級 14）") || ItemHasSub(items1[i], u8"（階級 8）");
			with16 += is16;
			low += isLow;
			const bool hit = f && HoldsFor(*f, items1[i]);
			if ((is16 && !hit) || (isLow && hit)) {
				if (!bad++) why = u8"第 " + Num((long long)i + 1) + u8" 件" + (is16 ? u8"（階級 16）沒中" : u8"（低階）誤中");
			}
		}
		check(f && bad == 0 && with16 >= 2 && low == 2,
		      u8"S2 PoE1 階級 ≥16 片段：（階級 16）的 " + Num(with16) + u8" 件命中、（階級 14）（階級 8）不中 -- " + (f ? *f : std::string("(null)")) +
		          (bad ? " -- " + why : std::string()));
	}
	// S3
	{
		const std::optional<std::string> f = mn->entries[mQty].fragment(Min(60), Lang::Zh);
		std::vector<int> qty;
		int bad = 0;
		std::string why;
		const std::string pre = u8"物品數量: +";
		for (size_t i = 0; i < items1.size(); i++) {
			int n = -1;
			for (const std::string& l : items1[i])
				if (l.rfind(pre, 0) == 0) n = std::atoi(l.c_str() + pre.size());
			qty.push_back(n);
			const bool hit = f && HoldsFor(*f, items1[i]);
			if (n < 0 || hit != (n >= 60)) {
				if (!bad++) why = u8"第 " + Num((long long)i + 1) + u8" 件 +" + Num(n) + "% " + (hit ? u8"命中" : u8"沒中");
			}
		}
		check(qty == std::vector<int>{68, 70, 84, 52, 61, 58, 55, 75, 192},
		      u8"S3 前提：PoE1 樣本的物品數量依序為 68/70/84/52/61/58/55/75/192");
		check(f && bad == 0, u8"S3 PoE1 物品數量 ≥60% 片段：+68/+70/+84/+61/+75/+192 命中、+58/+55/+52 不中 -- " +
		                         (f ? *f : std::string("(null)")) + (bad ? " -- " + why : std::string()));
	}
}

} // namespace

void RegexR10Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	const DWORD t0 = GetTickCount();
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "R10: load data: " + err);
		return;
	}
	std::map<std::string, Game> games;
	for (const char* g : {"poe1", "poe2"}) {
		Game& G = games[g];
		G.id = g;
		G.algo = AlgoPages(g, ds.Labels(g));
	}
	for (auto& kv : games) {
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == kv.first) kv.second.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : kv.second.algo) kv.second.pages.push_back({nullptr, &p});
	}
	GroupTests(games);
	line("");
	g_rarityLabels = ds.Labels("poe2");   // the D tests use PoE2 pages
	DedupTests(games);
	g_rarityLabels = nullptr;
	line("");
	FragmentTests(games, exeDir);
	line("    (R10 checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
