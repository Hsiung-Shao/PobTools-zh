// --regex-selftest, R4 part: the multi-page merge (RegexAlgo::Combine). Ports
// exile-appraiser regex/test/combine.test.ts (the multi-page half; the single-page
// half is in regex_selftest_r3.cpp) and checks Combine / EscapeTerm against
// regex_r4_golden.inc, which tools/regex_port/gen-golden-r4.ts produced by running
// combine.ts over our own Data files: combine.test.ts cases, random multi-page
// combinations with custom text and excludes, and every pair of corpus pages.
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
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r4_golden.inc"

using namespace RegexAlgo;
using json = nlohmann::json;
using RegexGen::Mode;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;

void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

// One game's pages as the panel builds them: corpus pages in data order, then
// the algorithmic pages (store.ts:116).
struct Game {
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

int IdxOf(const RegexPageDef& p, const std::string& id)
{
	for (int i = 0; i < (int)p.entries.size(); i++)
		if (p.entries[i].id == id) return i;
	return -1;
}
int IdxOf(const AlgoPage& p, const std::string& id)
{
	for (int i = 0; i < (int)p.entries.size(); i++)
		if (p.entries[i].def.id == id) return i;
	return -1;
}

Lang LangOf(const std::string& s) { return s == "en" ? Lang::En : Lang::Zh; }
Mode ModeOf(const std::string& s) { return s == "all" ? Mode::All : s == "none" ? Mode::None : Mode::Any; }

AlgoValue ValueFrom(const json& j)
{
	AlgoValue v;
	if (j.contains("min") && j["min"].is_number()) v.min = j["min"].get<double>();
	if (j.contains("max") && j["max"].is_number()) v.max = j["max"].get<double>();
	if (j.contains("choice") && j["choice"].is_string()) {
		v.choice = j["choice"].get<std::string>();
		v.hasChoice = true;
	}
	return v;
}

// Page corpora per (page, language), as the panel caches them.
struct CorpusCache {
	std::map<std::string, RegexGen::Corpus> m;
	const RegexGen::Corpus& Get(const RegexPageDef& p, Lang lang)
	{
		const std::string key = p.game + "/" + p.id + (lang == Lang::Zh ? "/zh" : "/en");
		auto it = m.find(key);
		if (it != m.end()) return it->second;
		RegexGen::Corpus& c = m[key];
		BuildPageCorpus(p, lang, c);
		return c;
	}
};

// gen-golden-r4.ts fnv1a over the UTF-8 bytes
uint32_t Fnv1a(const std::string& s)
{
	uint32_t h = 0x811c9dc5u;
	for (unsigned char b : s) h = (h ^ b) * 0x01000193u;
	return h;
}

json ConflictArrays(const CombineResult& r, size_t limit)
{
	json out = json::array();
	for (size_t i = 0; i < r.conflicts.size() && i < limit; i++) {
		const Conflict& c = r.conflicts[i];
		out.push_back(json::array({ConflictKindId(c.kind), c.page, c.entry, c.text}));
	}
	return out;
}

// The result as gen-golden-r4.ts resultJ serialised it.
json ResultJ(const CombineResult& r)
{
	json per = json::array();
	for (const PageContribution& c : r.perPage)
		per.push_back({{"id", c.id}, {"picked", c.picked}, {"length", c.length}, {"unresolved", c.unresolved}, {"fragments", c.fragments}});
	json custom = json::array();
	for (const CustomTerm& c : r.custom) custom.push_back({{"text", c.text}, {"term", c.term}});
	json ex = json::array();
	for (const ExcludeToken& x : r.excludes) ex.push_back({{"text", x.text}, {"token", x.token}});
	// JSON.stringify of the whole list: arrays only, so no key-order question;
	// nlohmann writes UTF-8 raw and escapes control characters like JS does.
	const std::string all = ConflictArrays(r, (size_t)-1).dump(-1, ' ', false, json::error_handler_t::replace);
	return {{"query", r.query}, {"length", r.length}, {"limit", r.limit}, {"verifyQuery", r.verifyQuery}, {"ok", r.ok},
	        {"perPage", per}, {"custom", custom}, {"excludes", ex},
	        {"customLength", r.customLength}, {"excludesLength", r.excludesLength},
	        {"conflicts", ConflictArrays(r, 8)}, {"conflictsN", r.conflicts.size()}, {"conflictsHash", Fnv1a(all)},
	        {"check", {{"ok", r.check.ok}, {"missing", r.check.missing}, {"extra", r.check.extra}, {"ambient", r.check.ambient}}}};
}

// ---- golden --------------------------------------------------------------------

void GoldenR4(const std::map<std::string, Game>& games)
{
	line("[R4-golden] exile-appraiser combine.ts over our Data files (regex_r4_golden.inc)");
	std::map<std::string, int> total, bad;
	std::map<std::string, std::string> firstBad;
	int parseErr = 0;
	CorpusCache cache;
	UnionCorpusCache unions;   // shared across records, as the panel keeps one
	auto fail = [&](const std::string& kind, const std::string& what) {
		if (!bad[kind]++) firstBad[kind] = what;
	};
	const DWORD t0 = GetTickCount();
	std::vector<std::pair<double, std::string>> slow;
	for (const char* rec : kRegexR4Golden) {
		json r = json::parse(rec, nullptr, false);
		if (r.is_discarded() || !r.is_array() || r.empty()) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		total[kind]++;
		if (kind == "escape") {
			const std::string got = EscapeTerm(r[1].get<std::string>());
			if (got != r[2].get<std::string>()) fail(kind, r[1].dump() + ": got " + json(got).dump() + " want " + r[2].dump());
		} else if (kind == "combine") {
			LARGE_INTEGER qf, qa, qb;
			QueryPerformanceFrequency(&qf);
			QueryPerformanceCounter(&qa);
			// [combine, game, label, lang, mode, sels, custom, excludes, expected]
			const Game& G = games.at(r[1].get<std::string>());
			const Lang lang = LangOf(r[3].get<std::string>());
			const Mode mode = ModeOf(r[4].get<std::string>());
			std::vector<ValueMap> values(r[5].size());
			std::vector<CombineSel> sels;
			bool ok = true;
			for (size_t k = 0; k < r[5].size(); k++) {
				const json& s = r[5][k];
				const std::string id = s["id"].get<std::string>();
				CombineSel sel;
				if (const AlgoPage* a = G.Algo(id)) {
					sel.page.algo = a;
					for (auto it = s["values"].begin(); it != s["values"].end(); ++it) values[k][it.key()] = ValueFrom(it.value());
					sel.values = &values[k];
				} else if (const RegexPageDef* c = G.Corpus(id)) {
					sel.page.corpus = c;
					sel.corpus = &cache.Get(*c, lang);
				} else {
					ok = false;
				}
				sel.picks = s["picks"].get<std::vector<int>>();
				sels.push_back(sel);
			}
			if (!ok) { fail(kind, "unknown page in " + r[5].dump()); continue; }
			const CombineResult res = Combine(lang, mode, sels, r[6].get<std::vector<std::string>>(),
			                                  r[7].get<std::vector<std::string>>(), &unions);
			const json got = ResultJ(res);
			QueryPerformanceCounter(&qb);
			slow.push_back({(double)(qb.QuadPart - qa.QuadPart) * 1000.0 / (double)qf.QuadPart,
			                r[1].get<std::string>() + " " + r[2].get<std::string>() + " " + r[3].get<std::string>() + " " + r[4].get<std::string>()});
			if (got != r[8]) {
				std::string diff;
				for (auto it = r[8].begin(); it != r[8].end(); ++it)
					if (!got.contains(it.key()) || got[it.key()] != it.value())
						diff += "\n      " + it.key() + ": got " + got.value(it.key(), json()).dump().substr(0, 300) +
						        "\n      " + std::string(it.key().size(), ' ') + "  want " + it.value().dump().substr(0, 300);
				fail(kind, r[2].get<std::string>() + " " + r[3].get<std::string>() + " " + r[4].get<std::string>() + diff);
			}
		} else {
			fail("unknown", kind);
		}
	}
	check(parseErr == 0 && !total.empty(), "fixture parses (" + Num(sizeof kRegexR4Golden / sizeof kRegexR4Golden[0]) + " records)");
	for (const auto& kv : total) {
		const int b = bad[kv.first];
		check(b == 0, kv.first + ": " + Num(kv.second - b) + " / " + Num(kv.second) + " identical" +
		              (b ? "\n      first: " + firstBad[kv.first] : std::string()));
	}
	if (bad.count("unknown")) check(false, "unknown record kind " + firstBad["unknown"]);
	line("    (R4 golden took " + Num((long long)(GetTickCount() - t0)) + " ms)");
	std::sort(slow.begin(), slow.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (size_t i = 0; i < slow.size() && i < 3; i++) line("      slowest: " + Num((long long)slow[i].first) + " ms " + slow[i].second);
}

// ---- combine.test.ts (multi-page) -----------------------------------------------

// combine.test.ts:19 unionVerify: the test builds its own union, not through the cache.
RegexGen::Check UnionVerify(const std::vector<std::pair<const RegexPageDef*, std::vector<int>>>& sels, Lang lang,
                            const std::string& query)
{
	std::vector<RegexGen::Entry> entries;
	RegexGen::Ambient amb;
	std::vector<int> selected;
	for (const auto& s : sels) {
		RegexGen::Corpus one;
		BuildPageCorpus(*s.first, lang, one);   // the same entryLines / pageAmbient as the panel
		const int off = (int)entries.size();
		for (size_t i = 0; i < one.Size(); i++) {
			RegexGen::Entry e = one.At(i);
			e.id = s.first->id + ":" + e.id;
			entries.push_back(std::move(e));
		}
		const bool zh = lang == Lang::Zh;
		const auto& p = *s.first;
		amb.lines.insert(amb.lines.end(), (zh ? p.ambientZh : p.ambientEn).begin(), (zh ? p.ambientZh : p.ambientEn).end());
		amb.nameLeft.insert(amb.nameLeft.end(), (zh ? p.namePrefixZh : p.namePrefixEn).begin(), (zh ? p.namePrefixZh : p.namePrefixEn).end());
		amb.nameRight.insert(amb.nameRight.end(), (zh ? p.nameSuffixZh : p.nameSuffixEn).begin(), (zh ? p.nameSuffixZh : p.nameSuffixEn).end());
		for (int i : s.second) selected.push_back(off + i);
	}
	RegexGen::Corpus c;
	c.Reset(std::move(entries), std::move(amb));
	return c.Verify(selected, query);
}

int CountUnescapedBang(const std::string& q)
{
	int n = 0;
	for (size_t i = 0; i < q.size(); i++)
		if (q[i] == '!' && (i == 0 || q[i - 1] != '\\')) n++;
	return n;
}

int CountChar(const std::string& q, char c)
{
	return (int)std::count(q.begin(), q.end(), c);
}

void CombineTests(const std::map<std::string, Game>& games)
{
	line("[R4-combine] multi-page merge, custom text, excludes (combine.test.ts)");
	CorpusCache cache;
	const Game& P1 = games.at("poe1");
	const Game& P2 = games.at("poe2");
	const RegexPageDef* mapMods = P1.Corpus("map_mods");
	const RegexPageDef* logbook = P1.Corpus("logbook_mods");
	const AlgoPage* mapNum = P1.Algo("map_numeric");
	const AlgoPage* vendor = P1.Algo("vendor_items");
	if (!mapMods || !logbook || !mapNum || !vendor) {
		check(false, "R4: poe1 map_mods / logbook_mods / map_numeric / vendor_items present");
		return;
	}
	auto corpusSel = [&](const RegexPageDef* p, std::vector<int> picks, Lang lang) {
		CombineSel s;
		s.page.corpus = p;
		s.corpus = &cache.Get(*p, lang);
		s.picks = std::move(picks);
		return s;
	};
	auto algoSel = [](const AlgoPage* p, std::vector<int> picks, const ValueMap* v) {
		CombineSel s;
		s.page.algo = p;
		s.picks = std::move(picks);
		s.values = v;
		return s;
	};

	// combine.test.ts:54 three map modifiers + tier >= 16 + quantity >= 80 + 6L
	for (Lang lang : {Lang::Zh, Lang::En}) {
		const std::string L = lang == Lang::Zh ? "zh" : "en";
		std::vector<int> picks;
		for (const char* id : {"MapMonsterFast2MapWorlds", "MapMonsterDamage2MapWorlds", "MapMonsterChaosDamage2MapWorlds"})
			picks.push_back(IdxOf(*mapMods, id));
		ValueMap nv;
		nv["tier"].min = 16;
		nv["quantity"].min = 80;
		ValueMap vv;
		vv["links"].choice = "6";
		vv["links"].hasChoice = true;
		const CombineResult r = Combine(lang, Mode::Any,
			{corpusSel(mapMods, picks, lang), algoSel(mapNum, {IdxOf(*mapNum, "tier"), IdxOf(*mapNum, "quantity")}, &nv),
			 algoSel(vendor, {IdxOf(*vendor, "links")}, &vv)});
		const RegexGen::Result single = cache.Get(*mapMods, lang).Build(picks, Mode::Any);
		const std::string tier = lang == Lang::Zh ? u8"\"階級 *(1[6-9]|[2-9][0-9])）\"" : "\"Tier (1[6-9]|[2-9][0-9])\\)\"";
		const std::string qty = lang == Lang::Zh ? u8"\"物品數量[:：] *\\+?([89][0-9]|[1-9][0-9]{2,}) *%\""
		                                         : u8"\"Item Quantity[:：] *\\+?([89][0-9]|[1-9][0-9]{2,}) *%\"";
		const std::string six = ".-.-.-.-.-.";
		check(r.query == single.query + " " + tier + " " + qty + " " + six, L + ": modifiers + tier + quantity + 6L in one string: " + r.query);
		check(r.length == RegexGen::CharCount(r.query) && r.ok && r.conflicts.empty() && r.check.ok,
		      L + ": length = code points, ok, no conflicts, Verify ok");
		const bool shape = r.perPage.size() == 3 && r.perPage[0].id == "map_mods" && r.perPage[0].picked == 3 &&
		                   r.perPage[1].id == "map_numeric" && r.perPage[1].picked == 2 &&
		                   r.perPage[2].id == "vendor_items" && r.perPage[2].picked == 1;
		check(shape, L + ": perPage = map_mods 3, map_numeric 2, vendor_items 1");
		if (shape) {
			int tokLen = 0;
			for (const std::string& t : single.tokens) tokLen += RegexGen::CharCount(t) + 1;
			check(r.perPage[0].length == tokLen &&
			      r.perPage[1].length == RegexGen::CharCount(tier) + 1 + RegexGen::CharCount(qty) + 1 &&
			      r.perPage[2].length == RegexGen::CharCount(six) + 1,
			      L + ": each page's contribution = its pieces + one separator each");
		}
		std::string rest = r.query;
		for (const std::string& t : {tier, qty, six}) {
			const size_t at = rest.find(" " + t);
			if (at != std::string::npos) rest.erase(at, t.size() + 1);
		}
		const RegexGen::Check v = UnionVerify({{mapMods, picks}}, lang, rest);
		check(rest == r.verifyQuery && v.ok && v.extra.empty() && v.ambient.empty(),
		      L + ": the query minus the algorithmic terms = verifyQuery, and a separately built union verifies it");
	}

	// combine.test.ts:96 None + exclude 反射 -> a single '!' term
	{
		// samplePicks(5, n, 4)
		std::vector<int> picks;
		unsigned s = 5;
		while (picks.size() < 4) {
			s = s * 1664525u + 1013904223u;
			const int i = (int)((s >> 8) % (unsigned)mapMods->entries.size());
			if (std::find(picks.begin(), picks.end(), i) == picks.end()) picks.push_back(i);
		}
		const CombineResult r = Combine(Lang::Zh, Mode::None, {corpusSel(mapMods, picks, Lang::Zh)}, {}, {u8"反射"});
		const std::string tail = u8"|反射\"";
		check(CountChar(r.query, '!') == 1 && r.query.rfind("\"!", 0) == 0 &&
		      r.query.size() >= tail.size() && r.query.compare(r.query.size() - tail.size(), tail.size(), tail) == 0 &&
		      r.excludes.size() == 1 && r.excludes[0].text == u8"反射" && r.excludes[0].token == u8"反射" && r.ok,
		      "None + exclude 反射 -> one \"!..|反射\" term: " + r.query);
	}
	// combine.test.ts:106 two corpus pages in None: still one '!' term; the exclude's own '!' escaped
	{
		const CombineResult r = Combine(Lang::Zh, Mode::None,
			{corpusSel(mapMods, {0, 5}, Lang::Zh), corpusSel(logbook, {10}, Lang::Zh)}, {}, {u8"反射", u8"!驚嘆"});
		check(CountUnescapedBang(r.query) == 1 && r.query.find(u8"|\\!驚嘆\"") != std::string::npos,
		      "two corpus pages in None + excludes -> one unescaped '!': " + r.query);
	}
	// combine.test.ts:117 PoE2 rare monsters >= N% vs the relic line: the strict fragment needs a colon
	if (const RegexPageDef* relic = P2.Corpus("relic_mods")) {
		const AlgoPage* wn = P2.Algo("waystone_numeric");
		bool hasLine = false;
		for (const RegexEntryDef& e : relic->entries)
			for (const std::string& t : e.zh) hasLine |= t.rfind(u8"稀有怪物", 0) == 0 && t.find("#%") != std::string::npos;
		int frag = 0;
		for (Lang lang : {Lang::Zh, Lang::En}) {
			const CombineResult r = Combine(lang, Mode::Any, {corpusSel(relic, {0}, lang), algoSel(wn, {IdxOf(*wn, "rare_monsters")}, nullptr)});
			for (const Conflict& c : r.conflicts) frag += c.kind == ConflictKind::Fragment;
		}
		check(hasLine && frag == 0, u8"relic page still has a 稀有怪物…#% line, and rare monsters >= N% does not hit it");
	} else {
		check(false, "poe2 relic_mods present");
	}
	// combine.test.ts:127 None with an algorithmic term: the '!' term is last and alone
	{
		ValueMap nv;
		nv["tier"].min = 16;
		const CombineResult r = Combine(Lang::Zh, Mode::None, {corpusSel(mapMods, {1, 2}, Lang::Zh), algoSel(mapNum, {0}, &nv)});
		const size_t t = r.query.find(u8"\"階級 ");
		check(CountChar(r.query, '!') == 1 && t != std::string::npos && t < r.query.find('!'),
		      "None + tier: tier term before the single '!' term: " + r.query);
	}

	// combine.test.ts:139 cross-page extra
	{
		int life = -1, blind = -1;
		for (int i = 0; i < (int)mapMods->entries.size() && life < 0; i++)
			if (!mapMods->entries[i].en.empty() && mapMods->entries[i].en[0] == "#% more Monster Life") life = i;
		for (int i = 0; i < (int)logbook->entries.size() && blind < 0; i++)
			if (!logbook->entries[i].en.empty() && logbook->entries[i].en[0] == "Monsters Blind on Hit") blind = i;
		const CombineResult r = Combine(Lang::Zh, Mode::Any, {corpusSel(mapMods, {life}, Lang::Zh), corpusSel(logbook, {blind}, Lang::Zh)});
		bool extra = false;
		for (const Conflict& c : r.conflicts) extra |= c.kind == ConflictKind::Extra && c.page == "logbook_mods";
		check(life >= 0 && blind >= 0 && extra && !r.ok, "a map token that also hits a logbook line -> extra conflict on logbook_mods");
		// the entry id in a union conflict is the page's own entry id, not "page:entry"
		bool plain = true;
		for (const Conflict& c : r.conflicts)
			if (c.kind == ConflictKind::Extra)
				plain &= (c.page == "map_mods" || c.page == "logbook_mods") &&
				         IdxOf(c.page == "map_mods" ? *mapMods : *logbook, c.entry) >= 0;
		check(plain, "union conflicts name (page id, entry id) without the union prefix");
	}

	// combine.test.ts:145 fragment conflict, here also through a two-page union
	{
		RegexPageDef fake;
		fake.id = "fake";
		fake.game = "poe1";
		fake.groups = {"g"};
		RegexEntryDef a;
		a.id = "a";
		a.zh = {u8"地圖掉落物品數量: +#%"};
		a.en = {"x"};
		RegexEntryDef b;
		b.id = "b";
		b.zh = {u8"怪物移動速度"};
		b.en = {"y"};
		fake.entries = {a, b};
		ValueMap vals;
		vals["quantity"].min = 80;
		for (int pages = 1; pages <= 2; pages++) {
			std::vector<CombineSel> sels;
			CombineSel f;
			f.page.corpus = &fake;   // corpus null: Combine builds it for the call
			f.picks = {1};
			sels.push_back(f);
			if (pages == 2) sels.push_back(corpusSel(mapMods, {0}, Lang::Zh));
			sels.push_back(algoSel(mapNum, {IdxOf(*mapNum, "quantity")}, &vals));
			const CombineResult r = Combine(Lang::Zh, Mode::Any, sels);
			bool found = false;
			for (const Conflict& x : r.conflicts)
				found |= x.kind == ConflictKind::Fragment && x.entry == "quantity" && x.page == "map_numeric" &&
				         x.text.find(u8"⇐ 地圖掉落物品數量: +") != std::string::npos;
			check(found && !r.ok, "fragment that hits a corpus line -> fragment conflict (" + Num(pages) + " corpus page(s))");
		}
	}

	// combine.test.ts:160 an exclude that hits a ticked modifier contradicts itself (any / all only)
	{
		int refl = -1;
		for (int i = 0; i < (int)mapMods->entries.size() && refl < 0; i++)
			if (!mapMods->entries[i].zh.empty() && mapMods->entries[i].zh[0].find(u8"反射") != std::string::npos) refl = i;
		int n[3] = {0, 0, 0};
		std::string page;
		for (Mode m : {Mode::Any, Mode::All, Mode::None}) {
			const CombineResult r = Combine(Lang::Zh, m, {corpusSel(mapMods, {refl}, Lang::Zh)}, {}, {u8"反射"});
			for (const Conflict& c : r.conflicts)
				if (c.kind == ConflictKind::Exclude) {
					n[(int)m]++;
					page = c.page;
				}
		}
		check(refl >= 0 && n[0] == 1 && n[1] == 1 && n[2] == 0 && page == "map_mods",
		      "exclude 反射 vs a ticked 反射 modifier: exclude conflict in any / all, none in none");
	}

	// combine.test.ts:165 the real data: every default fragment against all map-like pages at once
	{
		int bad = 0;
		std::string first;
		for (const char* g : {"poe1", "poe2"}) {
			const Game& G = games.at(g);
			for (Lang lang : {Lang::Zh, Lang::En}) {
				std::vector<CombineSel> base;
				for (const char* pid : {"map_mods", "logbook_mods", "waystone_mods", "tablet_mods"})
					if (const RegexPageDef* cp = G.Corpus(pid)) base.push_back(corpusSel(cp, {0}, lang));
				for (const AlgoPage& ap : G.algo) {
					std::vector<CombineSel> sels = base;
					std::vector<int> all;
					for (int i = 0; i < (int)ap.entries.size(); i++) all.push_back(i);
					sels.push_back(algoSel(&ap, all, nullptr));
					for (const Conflict& c : Combine(lang, Mode::Any, sels).conflicts)
						if (c.kind == ConflictKind::Fragment || c.kind == ConflictKind::Invalid)
							if (!bad++) first = ap.id + " " + c.text;
				}
			}
		}
		check(bad == 0, "every default numeric / vendor fragment vs the union of the map-like pages: no fragment / invalid" +
		                (bad ? " -- " + first : std::string()));
	}

	// combine.test.ts:181 invalid input
	{
		ValueMap badv;
		badv["tier"].min = 90;
		badv["tier"].max = 10;
		const CombineResult r = Combine(Lang::Zh, Mode::Any, {algoSel(mapNum, {0}, &badv)});
		check(r.query.empty() && !r.conflicts.empty() && r.conflicts[0].kind == ConflictKind::Invalid, "invalid input -> invalid conflict, no term");
	}

	// combine.test.ts:188 custom text
	{
		check(EscapeTerm("a.b (c)") == "a\\.b \\(c\\)" && EscapeTerm("\"x\"") == "x" && EscapeTerm("!neg") == "\\!neg",
		      "escapeTerm: regex syntax, quotes, leading '!'");
		check(EscapeTerm(u8"　全形　") == u8"全形" && EscapeTerm(u8" nb ") == "nb" && EscapeTerm("  \t ") == "" &&
		      EscapeTerm(u8"﻿x") == "x" && EscapeTerm(" \" !x \" ") == "\\!x",
		      "escapeTerm trims like JS String.prototype.trim (full-width space, NBSP, BOM), after dropping quotes");
		const CombineResult r = Combine(Lang::Zh, Mode::Any, {}, {u8"6 連結", u8"品質+20%"});
		check(r.query == u8"\"6 連結\" 品質\\+20%", "custom terms: escaped, each a term, quoted when spaced: " + r.query);
		check(r.custom.size() == 2 &&
		      r.customLength == RegexGen::CharCount(u8"\"6 連結\"") + 1 + RegexGen::CharCount(u8"品質\\+20%") + 1,
		      "custom length = each term + one separator");
		const CombineResult blank = Combine(Lang::Zh, Mode::Any, {}, {"   ", "\"\""}, {" "});
		check(blank.query.empty() && blank.custom.empty() && blank.excludes.empty(), "blank custom / excludes are dropped");
	}

	// Order of the parts: any, all (none here), algorithmic, custom, none-term with excludes.
	{
		ValueMap nv;
		nv["tier"].min = 16;
		const CombineResult r = Combine(Lang::Zh, Mode::Any, {corpusSel(mapMods, {3}, Lang::Zh), algoSel(mapNum, {0}, &nv)},
		                                {u8"自訂"}, {u8"排除"});
		const size_t a = r.query.find('"'), t = r.query.find(u8"\"階級"), c = r.query.find(u8" 自訂"), x = r.query.find(u8"\"!排除\"");
		check(a == 0 && t != std::string::npos && c != std::string::npos && x != std::string::npos && t < c && c < x &&
		      x + std::string(u8"\"!排除\"").size() == r.query.size() && r.excludesLength == RegexGen::CharCount(u8"排除") + 1,
		      "any: corpus term, algorithmic, custom, then the '!' term made of the excludes: " + r.query);
	}

	// Single page through Combine = CombineSingle = the page's Build() (R3 behaviour unchanged).
	{
		int diff = 0;
		for (Mode m : {Mode::Any, Mode::All, Mode::None})
			for (Lang lang : {Lang::Zh, Lang::En}) {
				const CombineSel s = corpusSel(logbook, {1, 4, 9}, lang);
				const CombineResult a = Combine(lang, m, {s});
				const CombineResult b = CombineSingle(lang, m, {s});
				const RegexGen::Result w = s.corpus->Build(s.picks, m);
				diff += !(a.query == w.query && b.query == w.query && a.verifyQuery == b.verifyQuery && a.ok == b.ok);
			}
		check(diff == 0, "one page: Combine == CombineSingle == Build() (3 modes x 2 languages)");
	}

	// The union cache: same answer with and without it, page order matters, at most 4 kept.
	{
		UnionCorpusCache u;
		int diff = 0;
		const std::vector<const RegexPageDef*> corp = {mapMods, logbook, P1.Corpus("map_mods")};
		for (int round = 0; round < 2; round++)
			for (Mode m : {Mode::Any, Mode::All, Mode::None}) {
				const std::vector<CombineSel> s1 = {corpusSel(mapMods, {0, 7}, Lang::Zh), corpusSel(logbook, {2, 11}, Lang::Zh)};
				const std::vector<CombineSel> s2 = {corpusSel(logbook, {2, 11}, Lang::Zh), corpusSel(mapMods, {0, 7}, Lang::Zh)};
				const json a = ResultJ(Combine(Lang::Zh, m, s1, {}, {u8"反射"}, &u));
				const json b = ResultJ(Combine(Lang::Zh, m, s1, {}, {u8"反射"}, nullptr));
				const CombineResult c = Combine(Lang::Zh, m, s2, {}, {u8"反射"}, &u);
				diff += a != b;
				// reversed page order: the same pages, so the same verdict on every pick
				diff += c.check.ok != a["check"]["ok"].get<bool>();
			}
		check(diff == 0, "union cache: identical results with and without the cache, either page order");
		for (int k = 0; k < 6; k++) {
			const RegexPageDef* p2 = k % 2 ? logbook : mapMods;
			u.Get({p2, k % 3 ? mapMods : logbook}, k < 3 ? Lang::Zh : Lang::En);
		}
		const auto& again = u.Get({mapMods, logbook}, Lang::Zh);
		check(again.offsets.size() == 2 && again.offsets[1] == (int)mapMods->entries.size() &&
		      again.owner.size() == mapMods->entries.size() + logbook->entries.size() &&
		      again.owner[0].first == "map_mods" && again.owner.back().first == "logbook_mods",
		      "union: offsets and owners follow the page order");
	}

	// What one panel recompute of a busy merged view costs.
	{
		ValueMap nv;
		std::vector<int> allSec;
		for (int i = 0; i < (int)mapNum->entries.size(); i++) allSec.push_back(i);
		UnionCorpusCache u;
		const std::vector<CombineSel> sels = {corpusSel(mapMods, {0, 1, 2}, Lang::Zh), algoSel(mapNum, allSec, &nv),
		                                      corpusSel(logbook, {3, 4}, Lang::Zh), algoSel(vendor, {0, 1}, nullptr)};
		Combine(Lang::Zh, Mode::Any, sels, {u8"自訂"}, {u8"反射"}, &u);   // warm the union
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		const int reps = 5;
		for (int k = 0; k < reps; k++) Combine(Lang::Zh, Mode::Any, sels, {u8"自訂"}, {u8"反射"}, &u);
		QueryPerformanceCounter(&t1);
		const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / reps;
		char buf[96];
		snprintf(buf, sizeof buf, "%.1f", ms);
		line(std::string("    (map_mods + full section + logbook + vendor + custom + exclude, cached union: ") + buf + " ms)");
	}
}

} // namespace

void RegexR4Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	const DWORD t0 = GetTickCount();
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "R4: load data: " + err);
		return;
	}
	std::map<std::string, Game> games;
	for (const char* g : {"poe1", "poe2"}) {
		Game& G = games[g];
		G.algo = AlgoPages(g, ds.Labels(g));
	}
	// PageRefs point into ds and games[g].algo; neither moves from here on.
	for (auto& kv : games) {
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == kv.first) kv.second.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : kv.second.algo) kv.second.pages.push_back({nullptr, &p});
	}
	CombineTests(games);
	line("");
	GoldenR4(games);
	line("    (R4 checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
