// --regex-selftest, R3 part: the numeric section, the vendor page and the single-
// page output (regex_algo_pages). Ports exile-appraiser regex/test/pages.test.ts,
// sections.test.ts (page composition, id mapping, condText / sectionSummary),
// strict-fragments.test.ts (rarity / tier, clipboard samples), combine.test.ts
// (single page) and checks every exported function against regex_r3_golden.inc,
// which tools/regex_port/gen-golden-r3.ts produced by running the TS over our own
// Data files.
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
#include <map>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r3_golden.inc"

using namespace RegexAlgo;
using json = nlohmann::json;
using RegexGen::Mode;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;

void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

// What the panel builds: one game's corpus pages in data order, then its
// algorithmic pages (store.ts:116 `cat.pages.push(...algoPages(...))`).
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
	const AlgoEntry* Entry(const std::string& pid, const std::string& eid) const
	{
		const AlgoPage* p = Algo(pid);
		if (!p) return nullptr;
		for (const AlgoEntry& e : p->entries)
			if (e.def.id == eid) return &e;
		return nullptr;
	}
};

Game MakeGame(const RegexDataset& ds, const std::string& g)
{
	Game out;
	out.id = g;
	out.algo = AlgoPages(g, ds.Labels(g));
	for (const RegexPageDef& p : ds.Pages())
		if (p.game == g) out.pages.push_back({&p, nullptr});
	for (const AlgoPage& p : out.algo) out.pages.push_back({nullptr, &p});
	return out;
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

json OptJ(const std::optional<std::string>& s) { return s ? json(*s) : json(nullptr); }

const char* KindId(RegexPageKind k)
{
	switch (k) {
	case RegexPageKind::Mods: return "mods";
	case RegexPageKind::Names: return "names";
	case RegexPageKind::Numeric: return "numeric";
	case RegexPageKind::Sockets: return "sockets";
	}
	return "?";
}

const char* InputKindId(InputKind k)
{
	switch (k) {
	case InputKind::Range: return "range";
	case InputKind::Select: return "select";
	case InputKind::Colors: return "colors";
	case InputKind::Count: return "count";
	}
	return "?";
}

const char* OpId(RangeOp o)
{
	return o == RangeOp::Ge ? "ge" : o == RangeOp::Le ? "le" : o == RangeOp::Range ? "range" : "?";
}

json ValueJ(const AlgoValue& v)
{
	json o = json::object();
	if (v.min) o["min"] = *v.min;
	if (v.max) o["max"] = *v.max;
	if (RegexFrag::HasChoice(v)) o["choice"] = v.choice;
	return o;
}

// The page as the TS golden serialised it (gen-golden-r3.ts 'page').
json PageJ(const AlgoPage& p)
{
	json entries = json::array();
	for (const AlgoEntry& e : p.entries) {
		json in = json::object();
		in["kind"] = InputKindId(e.input.kind);
		switch (e.input.kind) {
		case InputKind::Range: {
			in["digits"] = e.input.digits;
			in["percent"] = e.input.percent;
			json ops = json::array();
			for (RangeOp o : e.input.ops) ops.push_back(OpId(o));
			in["ops"] = ops;
			in["lo"] = e.input.lo;
			in["hi"] = e.input.hi;
			break;
		}
		case InputKind::Count:
			in["lo"] = e.input.lo;
			in["hi"] = e.input.hi;
			[[fallthrough]];
		case InputKind::Select: {
			json opts = json::array();
			for (const AlgoOption& o : e.input.options) opts.push_back({{"id", o.id}, {"zh", o.zh}, {"en", o.en}});
			in["options"] = opts;
			break;
		}
		case InputKind::Colors:
			in["maxTotal"] = e.input.maxTotal;
			break;
		}
		in["def"] = ValueJ(e.input.def);
		entries.push_back({{"id", e.def.id}, {"g", e.def.group}, {"zh", e.def.zh}, {"en", e.def.en},
		                   {"untested", e.untested}, {"own", (bool)e.ownLine}, {"input", in}});
	}
	return {{"id", p.id}, {"kind", KindId(p.kind)}, {"sectionOf", p.sectionOf}, {"title", p.title},
	        {"titleEn", p.titleEn}, {"note", p.note}, {"limit", p.limit}, {"groups", p.groups},
	        {"groupsEn", p.groupsEn}, {"entries", entries}};
}

// Key-order-insensitive structural equality (nlohmann's == already is, for objects).
bool SameJ(const json& a, const json& b) { return a == b; }

std::vector<std::string> Ids(const std::vector<PageRef>& v)
{
	std::vector<std::string> out;
	for (const PageRef& r : v) out.push_back(r.Id());
	return out;
}

// ---- corpus cache for combine -------------------------------------------------

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

json CombineJ(const CombineResult& r)
{
	json per = json::array();
	for (const PageContribution& c : r.perPage)
		per.push_back({{"id", c.id}, {"picked", c.picked}, {"length", c.length}, {"unresolved", c.unresolved}, {"fragments", c.fragments}});
	json con = json::array();
	for (const Conflict& c : r.conflicts)
		con.push_back({{"kind", ConflictKindId(c.kind)}, {"page", c.page}, {"entry", c.entry}, {"text", c.text}});
	return {{"query", r.query}, {"length", r.length}, {"limit", r.limit}, {"verifyQuery", r.verifyQuery}, {"ok", r.ok},
	        {"perPage", per}, {"conflicts", con},
	        {"check", {{"ok", r.check.ok}, {"missing", r.check.missing}, {"extra", r.check.extra}, {"ambient", r.check.ambient}}}};
}

// ---- golden --------------------------------------------------------------------

void GoldenR3(const std::map<std::string, Game>& games)
{
	line("[R3-golden] exile-appraiser outputs over our Data files (regex_r3_golden.inc)");
	std::map<std::string, int> total, bad;
	std::map<std::string, std::string> firstBad;
	int parseErr = 0;
	CorpusCache cache;
	auto fail = [&](const std::string& kind, const std::string& what) {
		if (!bad[kind]++) firstBad[kind] = what;
	};
	for (const char* rec : kRegexR3Golden) {
		json r = json::parse(rec, nullptr, false);
		if (r.is_discarded() || !r.is_array() || r.empty()) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		total[kind]++;
		const std::string g = r.size() > 1 && r[1].is_string() ? r[1].get<std::string>() : std::string();
		auto git = games.find(g);
		if (kind == "page") {
			const Game& G = git->second;
			const AlgoPage* p = G.Algo(r[2]["id"].get<std::string>());
			if (!p) { fail(kind, "missing page " + r[2]["id"].get<std::string>()); continue; }
			const json mine = PageJ(*p);
			if (!SameJ(mine, r[2])) fail(kind, p->id + ": got " + mine.dump() + "\n      want " + r[2].dump());
		} else if (kind == "listed") {
			const auto got = Ids(ListedPages(git->second.pages));
			if (json(got) != r[2]) fail(kind, g + ": " + json(got).dump());
		} else if (kind == "order") {
			std::vector<std::string> got;
			if (r[2].is_null()) got = Ids(CombineOrder(git->second.pages));
			else {
				const std::string only = r[2].get<std::string>();
				got = Ids(CombineOrder(git->second.pages, &only));
			}
			if (json(got) != r[3]) fail(kind, g + " " + r[2].dump() + ": " + json(got).dump());
		} else if (kind == "host") {
			const std::string id = r[2].get<std::string>();
			const AlgoPage* sec = SectionPageOf(git->second.pages, id);
			const json secJ = sec ? json(sec->id) : json(nullptr);
			if (HostIdOf(git->second.pages, id) != r[3].get<std::string>() || secJ != r[4]) fail(kind, g + " " + id);
		} else if (kind == "frag") {
			const AlgoEntry* e = git->second.Entry(r[2].get<std::string>(), r[3].get<std::string>());
			if (!e) { fail(kind, "missing entry " + r[3].dump()); continue; }
			const AlgoValue v = ValueFrom(r[4]);
			const Lang lang = LangOf(r[5].get<std::string>());
			const json f = OptJ(e->fragment(v, lang));
			const json c = OptJ(CondText(*e, v, lang));
			if (f != r[6] || c != r[7])
				fail(kind, r[2].get<std::string>() + "/" + e->def.id + " " + r[4].dump() + " " + r[5].get<std::string>() +
				     ": got " + f.dump() + " " + c.dump() + ", want " + r[6].dump() + " " + r[7].dump());
		} else if (kind == "own") {
			const AlgoEntry* e = git->second.Entry(r[2].get<std::string>(), r[3].get<std::string>());
			const bool got = e && e->ownLine && e->ownLine(r[4].get<std::string>());
			if (got != r[5].get<bool>()) fail(kind, r[4].get<std::string>());
		} else if (kind == "summary") {
			const AlgoPage* p = git->second.Algo(r[2].get<std::string>());
			ValueMap vals;
			for (auto it = r[4].begin(); it != r[4].end(); ++it) vals[it.key()] = ValueFrom(it.value());
			std::vector<int> picks = r[3].get<std::vector<int>>();
			std::reverse(picks.begin(), picks.end());   // the TS passes them reversed: order must not matter
			json got = json::array();
			for (const SummaryItem& s : SectionSummary(*p, picks, vals, LangOf(r[5].get<std::string>())))
				got.push_back({{"id", s.id}, {"label", s.label}, {"cond", OptJ(s.cond)}});
			if (got != r[6]) fail(kind, p->id + ": got " + got.dump() + " want " + r[6].dump());
		} else if (kind == "combine") {
			const Game& G = git->second;
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
			const json got = CombineJ(CombineSingle(lang, mode, sels));
			if (got != r[6]) fail(kind, r[2].get<std::string>() + " " + r[3].get<std::string>() + " " + r[4].get<std::string>() +
			                            "\n      got  " + got.dump() + "\n      want " + r[6].dump());
		} else if (kind == "clip") {
			// [clip, file, lang, lines, {key: [hit line indices]}]
			const std::string file = r[1].get<std::string>();
			const Lang lang = LangOf(r[2].get<std::string>());
			const std::vector<std::string> lines = r[3].get<std::vector<std::string>>();
			const bool poe2 = file.find("poe2/") != std::string::npos;
			const Game& G = games.at(poe2 ? "poe2" : "poe1");
			const AlgoPage* sec = G.Algo(poe2 ? "waystone_numeric" : "map_numeric");
			for (auto it = r[4].begin(); it != r[4].end(); ++it) {
				const std::string key = it.key();
				const size_t colon = key.find(':');
				const std::string eid = key.substr(0, colon) == "rarity" ? kRarityEntryId : "tier";
				const AlgoEntry* e = G.Entry(sec->id, eid);
				AlgoValue v;
				if (eid == "tier") v = ValueFrom(json::parse(key.substr(colon + 1)));
				else v.choice = key.substr(colon + 1);
				const std::optional<std::string> f = e ? e->fragment(v, lang) : std::nullopt;
				std::string err;
				const std::optional<Rx> rx = f ? RxCompile(*f, &err) : std::nullopt;
				std::vector<int> hits;
				if (rx)
					for (int i = 0; i < (int)lines.size(); i++)
						if (RxSearch(*rx, lines[i])) hits.push_back(i);
				if (json(hits) != it.value()) fail(kind, file + " " + key + ": got " + json(hits).dump() + " want " + it.value().dump());
			}
		} else {
			fail("unknown", kind);
		}
	}
	check(parseErr == 0 && !total.empty(), "fixture parses (" + Num(sizeof kRegexR3Golden / sizeof kRegexR3Golden[0]) + " records)");
	for (const auto& kv : total) {
		const int b = bad[kv.first];
		check(b == 0, kv.first + ": " + Num(kv.second - b) + " / " + Num(kv.second) + " identical" +
		              (b ? "\n      first: " + firstBad[kv.first] : std::string()));
	}
	if (bad.count("unknown")) check(false, "unknown record kind " + firstBad["unknown"]);
}

// ---- pages.test.ts / sections.test.ts --------------------------------------------

void CompositionTests(const RegexDataset& ds, const std::map<std::string, Game>& games)
{
	line("[R3-pages] algorithmic page composition (pages.test.ts, sections.test.ts)");
	for (const char* g : {"poe1", "poe2"}) {
		const RegexLabels* lab = ds.Labels(g);
		int missing = 0;
		std::string first;
		std::vector<std::string> keys = NumericLabelKeys(g);
		for (const std::string& k : VendorLabelKeys(g)) keys.push_back(k);
		for (const std::string& k : keys) {
			const std::string* zh = lab ? lab->Find(true, k) : nullptr;
			const std::string* en = lab ? lab->Find(false, k) : nullptr;
			if (!zh || !en || zh->empty() || en->empty() || zh->find('[') != std::string::npos || zh->find('{') != std::string::npos) {
				if (!missing++) first = k;
			}
		}
		check(missing == 0, std::string(g) + ": every label the algorithmic pages use is in regex_" + g + ".json (zh + en, " +
		                    Num((long long)keys.size()) + " keys)" + (missing ? " -- missing " + first : std::string()));

		const Game& G = games.at(g);
		std::vector<std::string> ids;
		for (const AlgoPage& p : G.algo) ids.push_back(p.id);
		const bool poe1 = std::string(g) == "poe1";
		check(ids == (poe1 ? std::vector<std::string>{"map_numeric", "vendor_items"}
		                   : std::vector<std::string>{"waystone_numeric", "vendor_items_poe2"}),
		      std::string(g) + ": numeric section + vendor page, in that order");
		int dupes = 0, noDefault = 0;
		for (const AlgoPage& p : G.algo) {
			std::set<std::string> seen;
			for (const AlgoEntry& e : p.entries) {
				if (!seen.insert(e.def.id).second) dupes++;
				if (!e.fragment(e.input.def, Lang::Zh) || !e.fragment(e.input.def, Lang::En)) noDefault++;
			}
		}
		check(dupes == 0 && noDefault == 0, std::string(g) + ": entry ids unique; every default value builds a fragment in both languages");

		// sections.test.ts:127
		const std::string host = poe1 ? "map_mods" : "waystone_mods";
		const std::string sec = poe1 ? "map_numeric" : "waystone_numeric";
		const std::string vendor = poe1 ? "vendor_items" : "vendor_items_poe2";
		const std::string other = poe1 ? "logbook_mods" : "tablet_mods";
		const std::vector<std::string> listed = Ids(ListedPages(G.pages));
		auto has = [&](const std::string& id) { return std::find(listed.begin(), listed.end(), id) != listed.end(); };
		const AlgoPage* secP = SectionPageOf(G.pages, host);
		const std::vector<std::string> order = Ids(CombineOrder(G.pages));
		auto pos = [&](const std::string& id) { return (int)(std::find(order.begin(), order.end(), id) - order.begin()); };
		check(!has(sec) && has(host) && has(vendor) && secP && secP->id == sec && !SectionPageOf(G.pages, other) &&
		      pos(sec) == pos(host) + 1 && pos(sec) < pos(vendor) &&
		      Ids(CombineOrder(G.pages, &host)) == std::vector<std::string>{host, sec} &&
		      Ids(CombineOrder(G.pages, &vendor)) == std::vector<std::string>{vendor},
		      std::string(g) + ": section not in the list, follows its host; single-page order = host + section");
	}
	check(SectionHostOf("map_numeric") == "map_mods" && SectionHostOf("map_mods").empty() && SectionHostOf("toString").empty() &&
	      SectionIdOf("waystone_mods") == "waystone_numeric" && NumericKeyOf("map_numeric") == "map_mods" &&
	      NumericKeyOf("vendor_items") == "vendor_items", "section id mapping (sections.ts)");
	check(NumericPages("poe1", nullptr).empty() && VendorPages("poe2", nullptr).empty(), "no labels (schema 1) = no algorithmic pages");
	{
		// A label set missing the rarity option keys drops the row, not the page.
		RegexLabels partial = *ds.Labels("poe1");
		auto drop = [](std::vector<std::pair<std::string, std::string>>& v, const std::string& k) {
			v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& kv) { return kv.first == k; }), v.end());
		};
		for (const char* k : {"ItemDisplayStringRarity", "ItemDisplayMapScarabDropBonus"}) {
			drop(partial.zh, k);
			drop(partial.en, k);
		}
		const std::vector<AlgoPage> p = NumericPages("poe1", &partial);
		bool hasRarity = false, hasScarabs = false;
		if (!p.empty())
			for (const AlgoEntry& e : p[0].entries) {
				hasRarity |= e.def.id == kRarityEntryId;
				hasScarabs |= e.def.id == "scarabs";
			}
		check(p.size() == 1 && !hasRarity && !hasScarabs && p[0].entries.size() == 7, "a missing label drops only its row (never a guessed text)");
	}

	// pages.test.ts:154 / :166 -- the actual strings.
	const Game& P1 = games.at("poe1");
	const Game& P2 = games.at("poe2");
	auto F = [](const Game& G, const char* page, const char* id, AlgoValue v, Lang lang) -> std::string {
		const AlgoEntry* e = G.Entry(page, id);
		const std::optional<std::string> f = e ? e->fragment(v, lang) : std::nullopt;
		return f ? *f : std::string("<null>");
	};
	auto mn = [](double a) { AlgoValue v; v.min = a; return v; };
	auto ch = [](const char* c) { AlgoValue v; v.choice = c; return v; };
	AlgoValue t14;
	t14.min = 14;
	t14.max = 14;
	check(F(P1, "map_numeric", "tier", mn(16), Lang::Zh) == u8"階級 *(1[6-9]|[2-9][0-9])）" &&
	      F(P1, "map_numeric", "tier", mn(16), Lang::En) == "Tier (1[6-9]|[2-9][0-9])\\)" &&
	      F(P1, "map_numeric", "quantity", mn(80), Lang::Zh) == u8"物品數量[:：] *\\+?([89][0-9]|[1-9][0-9]{2,}) *%" &&
	      F(P1, "map_numeric", "quantity", mn(80), Lang::En) == u8"Item Quantity[:：] *\\+?([89][0-9]|[1-9][0-9]{2,}) *%" &&
	      F(P1, "map_numeric", kRarityEntryId, ch("rare"), Lang::Zh) == u8"稀有度[:：] *稀有" &&
	      F(P1, "map_numeric", kRarityEntryId, ch("normal"), Lang::Zh) == u8"稀有度[:：] *普通" &&
	      F(P1, "map_numeric", kRarityEntryId, ch("unique"), Lang::En) == u8"Rarity[:：] *Unique",
	      "PoE1 tier >=16 / quantity >=80 / rarity: the step-35 strict strings");
	check(F(P2, "waystone_numeric", "pack", mn(20), Lang::Zh) == u8"怪群大小[:：] *\\+?([2-9][0-9]|[1-9][0-9]{2,}) *%" &&
	      F(P2, "waystone_numeric", "waystone_drop", mn(50), Lang::Zh) == u8"換界石掉落機率[:：] *\\+?([5-9][0-9]|[1-9][0-9]{2,}) *%" &&
	      F(P2, "waystone_numeric", "monster_rarity", mn(5), Lang::Zh) == u8"怪物稀有度[:：] *\\+?([5-9]|[1-9][0-9]{1,}) *%" &&
	      F(P2, "waystone_numeric", "rarity", mn(10), Lang::Zh) == u8"物品稀有度[:：] *\\+?[1-9][0-9]{1,} *%" &&
	      F(P2, "waystone_numeric", kRarityEntryId, ch("rare"), Lang::Zh) == u8"稀有度[:：] *稀有" &&
	      F(P2, "waystone_numeric", kRarityEntryId, ch("normal"), Lang::Zh) == u8"稀有度[:：] *中" &&
	      F(P2, "waystone_numeric", "tier", t14, Lang::Zh) == u8"階級 *14）",
	      "PoE2 pack / waystone drop / monster rarity / rarity / tier: same shape as the community strings");

	// sections.test.ts:369 condText / sectionSummary
	const AlgoPage* sec = P1.Algo("map_numeric");
	auto E = [&](const char* id) { return P1.Entry("map_numeric", id); };
	AlgoValue r50;
	r50.min = 50;
	r50.max = 120;
	AlgoValue le40;
	le40.max = 40;
	AlgoValue bad;
	bad.min = 90;
	bad.max = 10;
	check(CondText(*E("tier"), mn(16), Lang::Zh) == std::optional<std::string>(u8"≥16") &&
	      CondText(*E("quantity"), mn(80), Lang::Zh) == std::optional<std::string>(u8"≥80%") &&
	      CondText(*E("quantity"), le40, Lang::En) == std::optional<std::string>(u8"≤40%") &&
	      CondText(*E("rarity"), r50, Lang::Zh) == std::optional<std::string>(u8"50–120%") &&
	      !CondText(*E("rarity"), AlgoValue{}, Lang::Zh) && !CondText(*E("rarity"), bad, Lang::Zh),
	      "condText: >= / <= / range, percent, invalid");
	check(CondText(*E(kRarityEntryId), ch("rare"), Lang::Zh) == std::optional<std::string>(u8"稀有") &&
	      CondText(*E(kRarityEntryId), ch("normal"), Lang::Zh) == std::optional<std::string>(u8"普通") &&
	      CondText(*E(kRarityEntryId), ch("unique"), Lang::En) == std::optional<std::string>("Unique") &&
	      !CondText(*E(kRarityEntryId), ch("nope"), Lang::Zh) && E(kRarityEntryId)->input.def.choice == "rare",
	      "condText shows the option's text (not its id); unknown option = invalid; rarity defaults to rare");
	{
		int tierIdx = -1, qtyIdx = -1;
		for (int i = 0; i < (int)sec->entries.size(); i++) {
			if (sec->entries[i].def.id == "tier") tierIdx = i;
			if (sec->entries[i].def.id == "quantity") qtyIdx = i;
		}
		ValueMap vals;
		vals["quantity"] = mn(80);
		const auto got = SectionSummary(*sec, {qtyIdx, tierIdx}, vals, Lang::Zh);
		const auto en = SectionSummary(*sec, {tierIdx}, ValueMap{}, Lang::En);
		check(got.size() == 2 && got[0].id == "tier" && got[0].label == E("tier")->def.zh[0] &&
		      got[0].cond == std::optional<std::string>(u8"≥16") && got[1].cond == std::optional<std::string>(u8"≥80%") &&
		      en.size() == 1 && en[0].label == E("tier")->def.en[0] && SectionSummary(*sec, {}, vals, Lang::Zh).empty(),
		      "sectionSummary: ticked only, row order, label in the UI language");
	}
}

// ---- RegexAlgoList.vue / store.ts editing rules --------------------------------------

void EditorTests(const std::map<std::string, Game>& games)
{
	line("[R3-editor] row editing: operators, numbers, colours, auto-tick (RegexAlgoList.vue, store.ts setValue)");
	const Game& G = games.at("poe1");
	const AlgoPage* sec = G.Algo("map_numeric");
	const AlgoPage* vendor = G.Algo("vendor_items");
	const AlgoEntry& qty = *G.Entry("map_numeric", "quantity");
	const AlgoEntry& gem = *G.Entry("vendor_items", "gem_level");
	AlgoValue v = qty.input.def;   // {min: 80}
	check(OpOf(qty, v) == RangeOp::Ge && OpOf(qty, AlgoValue{}) == RangeOp::Ge, "opOf: from the value, else the first offered");
	AlgoValue le = WithOp(qty, v, RangeOp::Le);
	AlgoValue rg = WithOp(qty, v, RangeOp::Range);
	check(!le.min && le.max && *le.max == 80 && rg.min && rg.max && *rg.min == 80 && *rg.max == 80,
	      "setOp: <= keeps the number as max; range = (min, max) from what is there");
	AlgoValue lo;
	lo.max = 30;
	AlgoValue rg2 = WithOp(qty, lo, RangeOp::Range);
	check(rg2.min && rg2.max && *rg2.min == 30 && *rg2.max == 80, "setOp range from <=30: the missing min comes from the default (80), pair sorted");
	AlgoValue n1 = WithNum(qty, v, true, 120.0);
	check(n1.min && *n1.min == 80 && !n1.max, "setNum on a >= row: a max edit is dropped (operator kept)");
	AlgoValue n2 = WithNum(qty, rg, true, 95.7);
	AlgoValue n3 = WithNum(qty, rg, false, std::nullopt);
	check(n2.max && *n2.max == 95 && n2.min && *n2.min == 80 && !n3.min && n3.max, "setNum: truncates; a cleared field removes the bound");
	check(OpOf(gem, rg) == RangeOp::Ge, "opOf: a gem-level row only offers >=, whatever the value says");
	const AlgoEntry& colors = *G.Entry("vendor_items", "link_colors");
	AlgoValue c = colors.input.def;   // "rgb"
	c = WithColor(c, 'r', 2);
	c = WithColor(c, 'b', 0);
	check(c.choice == "rrg" && ColorCount(c, 'r') == 2 && ColorCount(c, 'g') == 1 && WithColor(c, 'g', 9).choice == "rrgggggg",
	      "setColor: letters by count r, g, b; each clamped to 0..6");
	AlgoValue z = WithColor(WithColor(WithColor(c, 'r', 0), 'g', 0), 'b', 0);
	check(z.choice.empty() && RegexFrag::HasChoice(z) && !colors.fragment(z, Lang::Zh), "all colours at 0 = invalid input");

	AlgoSelection s;
	s.Reset(sec->entries.size());
	const bool ticked = s.SetValue(*sec, 1, WithNum(qty, qty.input.def, false, 90.0));
	const bool again = s.SetValue(*sec, 1, WithNum(qty, s.values["quantity"], false, 91.0));
	check(ticked && !again && s.Count() == 1 && s.picked[1] && *ValueOf(s.values, qty).min == 91, "setValue ticks the row once; the value is kept");
	s.picked[1] = 0;
	check(*ValueOf(s.values, qty).min == 91 && ValueOf(s.values, *G.Entry("map_numeric", "pack")).min == 30.0,
	      "unticking keeps the edited value; untouched rows read their default");
	AlgoSelection v2;
	v2.Reset(vendor->entries.size());
	check(!v2.SetValue(*vendor, 0, WithChoice(v2.values["links"], "5"), false) && v2.Count() == 0, "setValue(tick = false) does not tick");
}

// ---- strict-fragments.test.ts ③ ④ (beyond the golden) -----------------------------

std::vector<std::string> CorpusLines(const RegexDataset& ds)
{
	std::set<std::string> out;
	static const char* const vals[] = {"1", "5", "14", "16", "20", "30", "50", "80", "100", "150"};
	for (const RegexPageDef& p : ds.Pages()) {
		std::vector<const std::string*> raw;
		for (const auto* list : {&p.ambientZh, &p.ambientEn}) for (const std::string& l : *list) raw.push_back(&l);
		for (const RegexEntryDef& e : p.entries)
			for (const auto* list : {&e.zh, &e.en, &e.hiddenZh, &e.hiddenEn}) for (const std::string& l : *list) raw.push_back(&l);
		for (const std::string* l : raw) {
			if (l->find('#') == std::string::npos) {
				out.insert(*l);
				continue;
			}
			for (const char* v : vals) {
				std::string t;
				for (char c : *l) {
					if (c == '#') t += v;
					else t += c;
				}
				out.insert(t);
			}
		}
	}
	return {out.begin(), out.end()};
}

void StrictTests(const RegexDataset& ds, const std::map<std::string, Game>& games)
{
	line("[R3-strict] rarity row and tier name (strict-fragments.test.ts)");
	const std::vector<std::string> lines = CorpusLines(ds);
	std::vector<std::u32string> cps(lines.size());
	for (size_t i = 0; i < lines.size(); i++) RxDecodeUtf8(lines[i], cps[i]);
	auto comp = [](const std::string& f) {
		std::string err;
		std::optional<Rx> r = RxCompile(f, &err);
		return r ? *r : Rx{};
	};
	for (const char* g : {"poe1", "poe2"}) {
		const Game& G = games.at(g);
		const AlgoPage* sec = G.Algo(std::string(g) == "poe1" ? "map_numeric" : "waystone_numeric");
		const AlgoEntry* e = G.Entry(sec->id, kRarityEntryId);
		if (!e) {
			check(false, std::string(g) + ": rarity row exists");
			continue;
		}
		std::vector<std::string> optIds;
		for (const AlgoOption& o : e->input.options) optIds.push_back(o.id);
		check(e->input.kind == InputKind::Select && optIds == std::vector<std::string>{"normal", "magic", "rare", "unique"},
		      std::string(g) + ": rarity row is the last select with normal / magic / rare / unique" +
		      (sec->entries.back().def.id == kRarityEntryId ? "" : " (NOT last)"));
		int wrong = 0, falseHits = 0, corpusHits = 0;
		std::string first;
		for (Lang lang : {Lang::Zh, Lang::En}) {
			const std::string label = lang == Lang::Zh ? e->def.zh[0] : e->def.en[0];
			const std::string itemR = lang == Lang::Zh ? u8"物品稀有度" : "Item Rarity";
			const std::string monR = lang == Lang::Zh ? u8"怪物稀有度" : "Monster Rarity";
			for (const AlgoOption& o : e->input.options) {
				AlgoValue v;
				v.choice = o.id;
				const Rx r = comp(*e->fragment(v, lang));
				for (const AlgoOption& opt : e->input.options)
					for (const char* sep : {": ", u8"：", ":", u8"： "})
						if (RxSearch(r, label + sep + (lang == Lang::Zh ? opt.zh : opt.en)) != (opt.id == o.id)) wrong++;
				for (int n = 0; n <= 999; n++)
					for (const std::string& lab : {itemR, monR})
						for (const std::string& l : {lab + ": +" + Num(n) + "%", lab + u8"：+" + Num(n) + "%", lab + ": " + Num(n) + "% (augmented)"})
							if (RxSearch(r, l)) falseHits++;
				for (size_t i = 0; i < cps.size(); i++)
					if (RxSearchCps(r, cps[i]) == RxStatus::Match) {
						if (!corpusHits++) first = lines[i];
					}
			}
		}
		check(wrong == 0 && falseHits == 0, std::string(g) + ": each option hits its own line in four separator forms, no other; "
		      "item / monster rarity 0-999 never");
		check(corpusHits == 0, std::string(g) + ": no rarity fragment hits any corpus line of either game (" + Num((long long)lines.size()) +
		      " lines)" + (corpusHits ? " -- " + first : std::string()));
	}

	// ④ tier: synthetic names 0..99
	const Game& P1 = games.at("poe1");
	const AlgoEntry& tier = *P1.Entry("map_numeric", "tier");
	struct C { std::optional<double> mn, mx; };
	const C conds[] = {{80, {}}, {30, {}}, {7, {}}, {0, {}}, {{}, 50}, {{}, 5}, {{}, 0}, {60, 86}, {9, 10}, {16, 16}};
	int wrong = 0;
	std::string first;
	for (const C& c : conds) {
		AlgoValue v;
		v.min = c.mn;
		v.max = c.mx;
		const Rx zh = comp(*tier.fragment(v, Lang::Zh));
		const Rx en = comp(*tier.fragment(v, Lang::En));
		for (int n = 0; n <= 99; n++) {
			const bool ok = (!c.mn || n >= *c.mn) && (!c.mx || n <= *c.mx);
			const std::string N = Num(n);
			for (const std::string& t : {u8"地圖（階級 " + N + u8"）", u8"凋落的 地圖（階級 " + N + u8"）", u8"換界石（階級 " + N + u8"）", u8"堅定的地圖（階級" + N + u8"）"})
				if (RxSearch(zh, t) != ok) { if (!wrong++) first = t; }
			for (const std::string& t : {"Map (Tier " + N + ")", "Blighted Map (Tier " + N + ")", "Waystone (Tier " + N + ")"})
				if (RxSearch(en, t) != ok) { if (!wrong++) first = t; }
		}
	}
	check(wrong == 0, "tier: synthetic names 0-99 x 10 conditions, zh + en" + (wrong ? " -- " + first : std::string()));
	{
		AlgoValue t;
		t.min = 16;
		const Rx r = comp(*tier.fragment(t, Lang::Zh));
		check(!RxSearch(r, u8"地圖（階級 14）\n--------\n怪物等級: 16）"), "tier: a non-matching name never borrows the next line's number");
	}
	{
		// Corpus lines other than the item names themselves never match a tier fragment.
		int hits = 0;
		std::string firstHit;
		std::string err;
		const std::optional<Rx> names = RxCompile(u8"（階級 *\\d+）|\\(Tier \\d+\\)", &err, RxFlags{false, false});
		std::vector<std::string> rest;
		for (const std::string& l : lines)
			if (!RxSearch(*names, l)) rest.push_back(l);
		rest.push_back(u8"{ 前綴 \"x\" (階級: 1) }");
		rest.push_back("{ Prefix Modifier \"Shocking\" (Tier: 1) }");
		rest.push_back(u8"換界石階級: 16");
		for (const char* mode : {"ge1", "le99"}) {
			AlgoValue v;
			if (std::string(mode) == "ge1") v.min = 1;
			else v.max = 99;
			for (Lang lang : {Lang::Zh, Lang::En}) {
				const Rx r = comp(*tier.fragment(v, lang));
				for (const std::string& l : rest)
					if (RxSearch(r, l)) { if (!hits++) firstHit = l; }
			}
		}
		check(hits == 0, "tier: no other corpus line, nor the advanced-mod \"(階級: 1)\" / \"換界石階級: 16\", matches" +
		                 (hits ? " -- " + firstHit : std::string()));
	}
}

// ---- combine.test.ts, single page ----------------------------------------------------

void CombineTests(const std::map<std::string, Game>& games)
{
	line("[R3-combine] single-page output (combine.test.ts)");
	CorpusCache cache;
	const Game& P1 = games.at("poe1");
	const RegexPageDef* mapMods = P1.Corpus("map_mods");
	// samplePicks (rng.ts): distinct indices in draw order
	auto sample = [](unsigned seed, int size, int count) {
		unsigned s = seed;
		auto next = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
		std::vector<int> sel;
		const int want = std::min(size, count);
		while ((int)sel.size() < want) {
			const int i = (int)(next() % (unsigned)size);
			if (std::find(sel.begin(), sel.end(), i) == sel.end()) sel.push_back(i);
		}
		return sel;
	};
	int diff = 0;
	std::string first;
	for (Mode mode : {Mode::Any, Mode::All, Mode::None}) {
		for (Lang lang : {Lang::Zh, Lang::En}) {
			const RegexGen::Corpus& c = cache.Get(*mapMods, lang);
			for (unsigned seed = 1; seed <= 20; seed++) {
				const std::vector<int> picks = sample(seed, (int)mapMods->entries.size(), 1 + (int)(seed % 7));
				const RegexGen::Result want = c.Build(picks, mode);
				CombineSel s;
				s.page.corpus = mapMods;
				s.corpus = &c;
				s.picks = picks;
				const CombineResult got = CombineSingle(lang, mode, {s});
				if (got.query != want.query || got.length != want.length || got.perPage.size() != 1 ||
				    got.perPage[0].unresolved != (int)want.unresolved.size()) {
					if (!diff++) first = got.query + " vs " + want.query;
				}
			}
		}
	}
	check(diff == 0, "one corpus page = Corpus.Build, character for character (3 modes x 2 languages x 20)" + (diff ? " -- " + first : std::string()));

	// The real data: every default fragment against the map-like corpus pages.
	int bad = 0;
	for (const char* g : {"poe1", "poe2"}) {
		const Game& G = games.at(g);
		for (const char* pid : {"map_mods", "logbook_mods", "waystone_mods", "tablet_mods"}) {
			const RegexPageDef* cp = G.Corpus(pid);
			if (!cp) continue;
			for (const AlgoPage& ap : G.algo) {
				for (Lang lang : {Lang::Zh, Lang::En}) {
					CombineSel a;
					a.page.corpus = cp;
					a.corpus = &cache.Get(*cp, lang);
					a.picks = {0};
					CombineSel b;
					b.page.algo = &ap;
					for (int i = 0; i < (int)ap.entries.size(); i++) b.picks.push_back(i);
					for (const Conflict& c : CombineSingle(lang, Mode::Any, {a, b}).conflicts)
						if (c.kind == ConflictKind::Fragment || c.kind == ConflictKind::Invalid) {
							if (!bad++) first = std::string(pid) + " " + c.text;
						}
				}
			}
		}
	}
	check(bad == 0, "all default numeric / vendor fragments: no fragment conflict with map / logbook / waystone / tablet lines" +
	                (bad ? " -- " + first : std::string()));
	{
		// What one panel recompute costs on the biggest host page with every
		// section row ticked (the fragment check runs on each edit).
		const AlgoPage* sec = P1.Algo("map_numeric");
		CombineSel a;
		a.page.corpus = mapMods;
		a.corpus = &cache.Get(*mapMods, Lang::Zh);
		a.picks = {0, 1, 2};
		CombineSel b;
		b.page.algo = sec;
		for (int i = 0; i < (int)sec->entries.size(); i++) b.picks.push_back(i);
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		const int reps = 5;
		for (int k = 0; k < reps; k++) CombineSingle(Lang::Zh, Mode::Any, {a, b});
		QueryPerformanceCounter(&t1);
		const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / reps;
		char buf[96];
		snprintf(buf, sizeof buf, "%.1f", ms);
		line(std::string("    (one map_mods + full numeric section output: ") + buf + " ms)");
	}

	{
		// A fake corpus line in the strict "label: +#%" shape is caught as a fragment conflict.
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
		RegexGen::Corpus c;
		BuildPageCorpus(fake, Lang::Zh, c);
		const AlgoPage* sec = P1.Algo("map_numeric");
		int qi = 0;
		while (sec->entries[qi].def.id != "quantity") qi++;
		ValueMap vals;
		vals["quantity"].min = 80;
		CombineSel s1;
		s1.page.corpus = &fake;
		s1.corpus = &c;
		s1.picks = {1};
		CombineSel s2;
		s2.page.algo = sec;
		s2.picks = {qi};
		s2.values = &vals;
		const CombineResult r = CombineSingle(Lang::Zh, Mode::Any, {s1, s2});
		bool found = false;
		for (const Conflict& x : r.conflicts) found |= (x.kind == ConflictKind::Fragment && x.entry == "quantity");
		check(found && !r.ok, "a fragment that also hits a corpus line is reported (fragment conflict)");

		ValueMap badv;
		badv["tier"].min = 90;
		badv["tier"].max = 10;
		CombineSel s3;
		s3.page.algo = sec;
		s3.picks = {0};
		s3.values = &badv;
		const CombineResult r2 = CombineSingle(Lang::Zh, Mode::Any, {s3});
		check(r2.query.empty() && !r2.conflicts.empty() && r2.conflicts[0].kind == ConflictKind::Invalid,
		      "invalid input -> invalid conflict, no term");
	}

	// combine.test.ts:54, single-page half: three map modifiers + tier >= 16 + quantity >= 80
	// in one string; the host's tokens come first, then each section term.
	{
		const AlgoPage* sec = P1.Algo("map_numeric");
		std::vector<int> picks;
		for (const char* id : {"MapMonsterFast2MapWorlds", "MapMonsterDamage2MapWorlds", "MapMonsterChaosDamage2MapWorlds"})
			for (int i = 0; i < (int)mapMods->entries.size(); i++)
				if (mapMods->entries[i].id == id) { picks.push_back(i); break; }   // findIndex: the first row
		ValueMap vals;
		vals["tier"].min = 16;
		vals["quantity"].min = 80;
		CombineSel a;
		a.page.corpus = mapMods;
		a.corpus = &cache.Get(*mapMods, Lang::Zh);
		a.picks = picks;
		CombineSel b;
		b.page.algo = sec;
		b.picks = {0, 1};
		b.values = &vals;
		const CombineResult r = CombineSingle(Lang::Zh, Mode::Any, {a, b});
		const std::string want = a.corpus->Build(picks, Mode::Any).query + u8" \"階級 *(1[6-9]|[2-9][0-9])）\" \"物品數量[:：] *\\+?([89][0-9]|[1-9][0-9]{2,}) *%\"";
		int sum = 0;
		for (const PageContribution& c : r.perPage) sum += c.length;
		check(picks.size() == 3 && r.query == want && r.ok && r.check.ok && r.perPage.size() == 2 &&
		      r.length == sum + 1,   // + the any-term's two quotes - the trailing separator
		      "map modifiers + tier >=16 + quantity >=80 -> one string: " + r.query);
	}
}

} // namespace

void RegexR3Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	const DWORD t0 = GetTickCount();
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "R3: load data: " + err);
		return;
	}
	std::map<std::string, Game> games;
	for (const char* g : {"poe1", "poe2"}) games.emplace(g, MakeGame(ds, g));
	// PageRefs point into ds and into games[g].algo; neither moves from here on.
	for (auto& kv : games) {
		kv.second.pages.clear();
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == kv.first) kv.second.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : kv.second.algo) kv.second.pages.push_back({nullptr, &p});
	}
	CompositionTests(ds, games);
	line("");
	EditorTests(games);
	line("");
	GoldenR3(games);
	line("");
	StrictTests(ds, games);
	line("");
	CombineTests(games);
	line("    (R3 checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
