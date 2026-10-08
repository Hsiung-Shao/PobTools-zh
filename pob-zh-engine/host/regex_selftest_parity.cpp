// --regex-selftest, parity part: the literal vectors exile-appraiser's OWN tests
// spell out (regex/test/{numeric,pages,strict-fragments,rarity,combine,item-mods,
// share}.test.ts), replayed from regex_parity_golden.inc. The generator
// (tools/regex_port/gen-parity.mts, local only) re-ran each through exile-
// appraiser's code over the very Data files shipped here and refused to write a
// vector whose literal that code no longer produces -- so every record is both
// what A's tests assert and what A's code does, and this file requires the C++
// port to give exactly the same output.
//
// Plus what only exists here: the item-mod page's keys written before it moved
// to trade stat ids (GGPK stat ids) are reported as missed on that page, never
// dropped and never mistaken for a trade id.
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_frag.h"
#include "regex_itemmods.h"
#include "regex_numeric.h"
#include "regex_share.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_parity_golden.inc"

using namespace RegexAlgo;
using json = nlohmann::json;
namespace IM = RegexItemMods;
namespace S = RegexShare;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;
void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

Lang LangOf(const std::string& s) { return s == "en" ? Lang::En : Lang::Zh; }
RegexGen::Mode ModeOf(const std::string& s)
{
	return s == "all" ? RegexGen::Mode::All : s == "none" ? RegexGen::Mode::None : RegexGen::Mode::Any;
}

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
RegexNumeric::NumRange RangeFrom(const json& j)
{
	RegexNumeric::NumRange r;
	if (j.contains("min")) r.min = j["min"].get<double>();
	if (j.contains("max")) r.max = j["max"].get<double>();
	return r;
}
json Opt(const std::optional<std::string>& s) { return s ? json(*s) : json(nullptr); }
json Opt(const std::optional<std::vector<std::string>>& s) { return s ? json(*s) : json(nullptr); }

const char* const kCatIds[IM::kCategoryCount] = {"life", "mana", "es", "resist", "attr", "speed", "damage", "move", "other"};

struct Game {
	std::vector<AlgoPage> algo;
	std::vector<PageRef> pages;
	std::optional<IM::Data> items;   // loaded on first use
	std::optional<AlgoPage> itemPage;
	const AlgoPage* Algo(const std::string& id) const
	{
		for (const AlgoPage& p : algo)
			if (p.id == id) return &p;
		return nullptr;
	}
	const RegexPageDef* Corpus(const std::string& id) const
	{
		for (const PageRef& r : pages)
			if (r.corpus && r.corpus->id == id) return r.corpus;
		return nullptr;
	}
};

const AlgoEntry* EntryOf(const AlgoPage* p, const std::string& id)
{
	if (!p) return nullptr;
	for (const AlgoEntry& e : p->entries)
		if (e.def.id == id) return &e;
	return nullptr;
}

} // namespace

void RegexParityTests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	line("--- parity: the literal vectors of exile-appraiser's own tests (regex_parity_golden.inc) ---");
	const DWORD t0 = GetTickCount();
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "parity: load data: " + err);
		return;
	}
	std::map<std::string, Game> games;
	for (const char* g : {"poe1", "poe2"}) games[g].algo = AlgoPages(g, ds.Labels(g));
	for (auto& kv : games) {
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == kv.first) kv.second.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : kv.second.algo) kv.second.pages.push_back({nullptr, &p});
	}
	auto items = [&](const std::string& g) -> const IM::Data* {
		Game& G = games[g];
		if (!G.items) {
			IM::Data d;
			std::string e;
			if (!IM::LoadFile(exeDir, g, d, &e)) {
				check(false, "parity: load item-mod data " + g + ": " + e);
				G.items = IM::Data();
			} else {
				G.items = std::move(d);
			}
		}
		return &*G.items;
	};

	std::map<std::string, int> total, bad;
	std::map<std::string, std::string> firstBad;
	int parseErr = 0;
	auto fail = [&](const std::string& kind, const std::string& what) {
		if (!bad[kind]++) firstBad[kind] = what;
	};
	for (const char* rec : kRegexParityGolden) {
		json r = json::parse(rec, nullptr, false);
		if (r.is_discarded() || !r.is_array() || r.empty()) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		total[kind]++;
		auto expect = [&](const json& got, const json& want, const std::string& what) {
			if (got != want) fail(kind, what + ": got " + got.dump() + " want " + want.dump());
		};
		if (kind == "range") {
			expect(RegexNumeric::RangeRegex(RangeFrom(r[1]), r[2].get<int>()), r[3], r[1].dump());
		} else if (kind == "readable") {
			expect(RegexNumeric::ReadableRangeRegex(RangeFrom(r[1]), r[2].get<int>(), r[3].get<bool>()), r[4], r[1].dump());
		} else if (kind == "frag" || kind == "terms" || kind == "condText") {
			// [kind, game, page, entry, value, lang, expected]
			const AlgoEntry* e = EntryOf(games[r[1].get<std::string>()].Algo(r[2].get<std::string>()), r[3].get<std::string>());
			if (!e) {
				fail(kind, "no entry " + r[2].dump() + "/" + r[3].dump());
				continue;
			}
			const AlgoValue v = ValueFrom(r[4]);
			const Lang lang = LangOf(r[5].get<std::string>());
			json got;
			if (kind == "frag") got = Opt(e->fragment(v, lang));
			else if (kind == "terms") got = e->terms ? Opt(e->terms(v, lang)) : json("<no terms>");
			else got = Opt(CondText(*e, v, lang));
			expect(got, r[6], r[2].get<std::string>() + "/" + r[3].get<std::string>() + " " + r[4].dump() + " " + r[5].get<std::string>());
		} else if (kind == "linkColors") {
			expect(Opt(RegexFrag::LinkColors(r[1].get<std::string>())), r[2], r[1].dump());
		} else if (kind == "wholeLine") {
			expect(Opt(RegexFrag::WholeLine(r[1].get<std::vector<std::string>>())), r[2], r[1].dump());
		} else if (kind == "rarityParse") {
			const RarityChoice c = ParseRarityChoice(r[1].is_null() ? std::string() : r[1].get<std::string>());
			expect(json{{"rarity", c.rarity}, {"corruption", CorruptionId(c.corruption)}}, r[2], r[1].dump());
		} else if (kind == "rarityEncode") {
			RarityChoice c;
			c.rarity = r[1]["rarity"].get<std::vector<std::string>>();
			const std::string co = r[1]["corruption"].get<std::string>();
			c.corruption = co == "corrupted" ? Corruption::Corrupted : co == "uncorrupted" ? Corruption::Uncorrupted : Corruption::None;
			expect(EncodeRarityChoice(c), r[2], r[1].dump());
		} else if (kind == "toggleRarity") {
			expect(ToggleRarityIn(r[1].get<std::string>(), r[2].get<std::string>()), r[3], r[1].dump() + " " + r[2].dump());
		} else if (kind == "toggleCorruption") {
			const std::string co = r[2].get<std::string>();
			expect(ToggleCorruptionIn(r[1].get<std::string>(), co == "corrupted" ? Corruption::Corrupted : Corruption::Uncorrupted), r[3],
			       r[1].dump() + " " + co);
		} else if (kind == "escape") {
			expect(EscapeTerm(r[1].get<std::string>()), r[2], r[1].dump());
		} else if (kind == "combine") {
			// [combine, game, label, lang, mode, sels, custom, excludes, {query, perPage, conflicts}]
			const Game& G = games[r[1].get<std::string>()];
			const Lang lang = LangOf(r[3].get<std::string>());
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
				} else {
					ok = false;
				}
				sel.picks = s["picks"].get<std::vector<int>>();
				sels.push_back(sel);
			}
			if (!ok) {
				fail(kind, "unknown page in " + r[5].dump());
				continue;
			}
			const CombineResult res = Combine(lang, ModeOf(r[4].get<std::string>()), sels, r[6].get<std::vector<std::string>>(),
			                                  r[7].get<std::vector<std::string>>());
			const json& want = r[8];
			json conflicts = json::array();
			for (const Conflict& c : res.conflicts) conflicts.push_back(ConflictKindId(c.kind));
			json per = nullptr;
			if (!want["perPage"].is_null()) {
				for (const PageContribution& c : res.perPage)
					if (c.id == want["perPage"]["id"].get<std::string>())
						per = {{"id", c.id}, {"fragments", c.fragments}, {"length", c.length}};
			}
			expect(json{{"query", res.query}, {"perPage", per}, {"conflicts", conflicts}}, want, r[2].get<std::string>());
		} else if (kind == "itemSummary") {
			const IM::Data* d = items(r[1].get<std::string>());
			json ex = json::object();
			for (int i = 0; i < IM::kReasonCount; i++) ex[IM::ReasonId(i)] = d->excluded[i];
			expect(json{{"itemStats", d->itemStats}, {"entries", d->entries.size()}, {"merged", d->merged}, {"excluded", ex}}, r[2],
			       r[1].get<std::string>());
		} else if (kind == "itemCommon") {
			// [itemCommon, game, ref, id, m, zh, en, cat]
			const IM::Data* d = items(r[1].get<std::string>());
			const IM::Entry* e = nullptr;
			for (const IM::Entry& x : d->entries)
				if (x.ref == r[2].get<std::string>()) e = &x;
			if (!e) {
				fail(kind, "no entry " + r[2].dump());
				continue;
			}
			AlgoValue v;
			v.min = r[4].get<double>();
			expect(json::array({e->id, Opt(IM::Fragment(e->Anchor(Lang::Zh), v)), Opt(IM::Fragment(e->Anchor(Lang::En), v)), kCatIds[(int)e->cat]}),
			       json::array({r[3], r[5], r[6], r[7]}), r[2].get<std::string>());
		} else if (kind == "itemCat") {
			expect(kCatIds[(int)IM::CategoryOf(r[1].get<std::string>())], r[2], r[1].dump());
		} else if (kind == "itemFrag") {
			// [itemFrag, game, entry id, value, lang, expected]
			const IM::Data* d = items(r[1].get<std::string>());
			const IM::Entry* e = nullptr;
			for (const IM::Entry& x : d->entries)
				if (x.id == r[2].get<std::string>()) e = &x;
			if (!e) {
				fail(kind, "no entry " + r[2].dump());
				continue;
			}
			expect(Opt(IM::Fragment(e->Anchor(LangOf(r[4].get<std::string>())), ValueFrom(r[3]))), r[5], r[3].dump());
		} else if (kind == "shareResolve") {
			// [shareResolve, game, code, {unknownPages, missed, picks:{page: n}, quantity}]
			S::Normalized n;
			std::string e;
			if (!S::Decode(r[2].get<std::string>(), n, &e)) {
				fail(kind, "decode: " + e);
				continue;
			}
			const S::Resolved res = S::Resolve(n.state, games[r[1].get<std::string>()].pages);
			json picks = json::object();
			for (auto it = r[3]["picks"].begin(); it != r[3]["picks"].end(); ++it) {
				auto p = res.picks.find(it.key());
				picks[it.key()] = p == res.picks.end() ? -1 : (int)p->second.size();
			}
			json q = nullptr;
			for (const auto& kv : res.values)
				if (kv.first == "map_numeric")
					for (const auto& ev : kv.second)
						if (ev.first == "quantity") {
							q = json::object();
							if (ev.second.min) q["min"] = *ev.second.min;
							if (ev.second.max) q["max"] = *ev.second.max;
						}
			expect(json{{"unknownPages", res.unknownPages}, {"missed", res.missed}, {"picks", picks}, {"quantity", q}}, r[3], "resolve");
		} else if (kind == "shareError") {
			// [shareError, label, code, prefixOnly, message]
			S::Normalized n;
			std::string e;
			const bool ok = S::Decode(r[2].get<std::string>(), n, &e);
			if (ok) {
				fail(kind, r[1].get<std::string>() + ": decoded");
				continue;
			}
			if (r[3].get<bool>()) e = e.substr(0, e.find('(') == std::string::npos ? e.size() : e.find('(') + 1);
			expect(e, r[4], r[1].get<std::string>());
		} else if (kind == "shareErrorLong") {
			S::Normalized n;
			std::string e;
			const bool ok = S::Decode(std::string((size_t)r[1].get<long long>(), 'A'), n, &e);
			expect(ok ? std::string("<decoded>") : e, r[2], "too long");
		} else {
			fail("unknown", kind);
		}
	}
	check(parseErr == 0 && !total.empty(), "parity fixture parses (" + Num((long long)(sizeof kRegexParityGolden / sizeof kRegexParityGolden[0])) + " records)");
	for (const auto& kv : total) {
		const int b = bad[kv.first];
		check(b == 0, "parity " + kv.first + ": " + Num(kv.second - b) + " / " + Num(kv.second) + u8" 與 exile-appraiser 逐字相同" +
		                  (b ? "\n      first: " + firstBad[kv.first] : std::string()));
	}
	if (bad.count("unknown")) check(false, "unknown record kind " + firstBad["unknown"]);

	// ---- legacy item-mod keys (GGPK stat ids, before 2026-10-09) ----------------------
	check(IM::IsLegacyKey("base_maximum_life") && IM::IsLegacyKey("stat_") && IM::IsLegacyKey("stat_12a") &&
	          IM::IsLegacyKey("local_display_supported_by_level_10_intensify") && IM::IsLegacyKey("action_speed_is_at_least_90%") &&
	          !IM::IsLegacyKey("stat_3299347043") && !IM::IsLegacyKey("stat_3299347043|+# to maximum Life") &&
	          !IM::IsLegacyKey("indexable_skill_83") && !IM::IsLegacyKey("pseudo_built_in_support|3") && !IM::IsLegacyKey("stat_1|2|3"),
	      u8"IsLegacyKey：GGPK stat id 判為舊鍵、交易站 id（stat_N / indexable_skill_N / pseudo_…|N，可帶 |ref）不是");
	{
		int legacyIds = 0, n = 0;
		for (const char* g : {"poe1", "poe2"})
			for (const IM::Entry& e : items(g)->entries) {
				n++;
				legacyIds += IM::IsLegacyKey(e.id) ? 1 : 0;
			}
		check(n > 0 && legacyIds == 0, u8"目前兩遊戲物品詞綴頁的全部 id（" + Num(n) + u8" 個）都不被判為舊鍵：" + Num(legacyIds));
	}
	{
		Game& G = games["poe1"];
		const IM::Data* d = items("poe1");
		if (!d->entries.empty()) {
			G.itemPage = IM::MakePage("poe1", d);
			std::vector<PageRef> pages = G.pages;
			pages.push_back({nullptr, &*G.itemPage});
			S::State st;
			st.game = "poe1";
			st.pages.push_back({IM::PageId("poe1"), {"base_maximum_life", d->entries[0].id, "base_fire_damage_resistance_%"}});
			const S::Resolved res = S::Resolve(st, pages);
			int reported = -1;
			for (const auto& kv : res.missedByPage)
				if (kv.first == IM::PageId("poe1")) reported = kv.second;
			auto p = res.picks.find(IM::PageId("poe1"));
			check(res.missed == 2 && reported == 2 && p != res.picks.end() && p->second == std::vector<int>{0},
			      u8"物品詞綴頁的舊鍵（GGPK stat id）回報為找不到、歸在該頁，新鍵照常還原：missed " + Num(res.missed) + u8"、該頁 " + Num(reported));
		}
	}
	line("    (parity checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
