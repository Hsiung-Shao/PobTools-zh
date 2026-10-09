// --regex-selftest, R7 part: the item-mod values page (regex_itemmods).
//
//   * load Data\regex_stats\<game>\{cmn-Hant,en}\stats.ndjson.gz the way the panel does (time + memory);
//   * regex_r7_golden.inc: exile-appraiser's own item-mods.ts / combine.ts run over the
//     same stats.ndjson (tools/regex_port/gen-golden-r7.ts) -- counts, every entry and anchor
//     (hashed + verbatim samples), every fragment over 16 conditions, chooseAnchor with
//     other limits, categories, filterItemMods, and merges with the map pages;
//   * item-mods.test.ts (1) uniqueness, FULL: every selectable entry x language, its
//     fragment for "any value" ({min: 0}, as item-mods-marker.test.ts T4), against every
//     line of that language's corpus (each '#' replaced by 16 values, with / without '+',
//     each also with the line-end marker " (fractured)") with the R1 matcher: it may hit
//     only its own text / its own stat's non-negated lines;
//   * item-mods.test.ts (2) own template value by value: every entry x language, one
//     condition 0..999 (+ 1000..12345) each, the other two at the edges (the edges also
//     with " (fractured)" appended: item-mods-marker.test.ts T4, same verdict);
//   * the PoE1 common ten (item-mods.test.ts:263) spelled out, page shape, bookmark round trip.
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_embed.h"
#include "regex_frag.h"
#include "regex_itemmods.h"
#include "regex_match.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r7_golden.inc"

using json = nlohmann::json;
using RegexFrag::AlgoValue;
using RegexFrag::Lang;
namespace IM = RegexItemMods;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;
void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

uint32_t Fnv1a(const std::string& s)
{
	uint32_t h = 0x811c9dc5u;
	for (unsigned char b : s) h = (h ^ b) * 0x01000193u;
	return h;
}
std::string Dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

const char* CatId(IM::Category c)
{
	static const char* const ids[] = {"life", "mana", "es", "resist", "attr", "speed", "damage", "move", "other"};
	return ids[(int)c];
}
// gen-golden-r7.ts aj(): 6 elements, plus [side, at, len, opts] only when the anchor carries `alt` (B 00bb297).
json AJ(const IM::ModAnchor& a)
{
	json j = json::array({a.p, a.s, a.caret, a.dollar, a.plus, a.cost});
	if (a.alt) j.push_back(json::array({std::string(1, a.alt->side), a.alt->at, a.alt->len, a.alt->opts}));
	return j;
}
json EJ(const IM::Entry& e)
{
	return json::array({e.id, e.ref, e.zh, e.en, CatId(e.cat), e.percent, AJ(e.anchors[0]), AJ(e.anchors[1])});
}
AlgoValue ValueFrom(const json& j)
{
	AlgoValue v;
	if (j.contains("min") && j["min"].is_number()) v.min = j["min"].get<double>();
	if (j.contains("max") && j["max"].is_number()) v.max = j["max"].get<double>();
	return v;
}
Lang LangOf(const std::string& s) { return s == "en" ? Lang::En : Lang::Zh; }

size_t PrivateBytes()
{
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof pmc;
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) return 0;
	return pmc.PrivateUsage;
}

struct GameData {
	IM::Data data;
	std::vector<IM::StatLite> zh, en;
	IM::ModIndexPtr zhIdx, enIdx;
	RegexAlgo::AlgoPage page;
	bool ok = false;
};

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size();
	}
	CloseHandle(h);
	return ok;
}

// ---- golden -------------------------------------------------------------------------

void GoldenTests(std::map<std::string, GameData>& G, const RegexDataset& ds)
{
	std::map<std::string, int> total, bad;
	std::map<std::string, std::string> firstBad;
	int parseErr = 0;
	auto fail = [&](const std::string& kind, const std::string& msg) {
		if (bad[kind]++ == 0) firstBad[kind] = msg.substr(0, 900);
	};
	const DWORD t0 = GetTickCount();
	for (const char* rec : kRegexR7Golden) {
		const json r = json::parse(rec, nullptr, false);
		if (r.is_discarded() || !r.is_array() || r.empty()) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		total[kind]++;
		if (kind == "cat") {
			const std::string got = CatId(IM::CategoryOf(r[1].get<std::string>()));
			if (got != r[2].get<std::string>()) fail(kind, r[1].dump() + " got " + got);
			continue;
		}
		GameData& g = G[r[1].get<std::string>()];
		if (!g.ok) {
			fail(kind, "game data not loaded");
			continue;
		}
		if (kind == "data") {
			const json& w = r[2];
			json got = {{"itemStats", g.data.itemStats}, {"merged", g.data.merged}, {"n", g.data.entries.size()}};
			json all = json::array();
			for (const IM::Entry& e : g.data.entries) all.push_back(EJ(e));
			got["hash"] = Fnv1a(Dump(all));
			std::string diff;
			for (const char* k : {"itemStats", "merged", "n", "hash"})
				if (got[k] != w[k]) diff += std::string(" ") + k + " got " + got[k].dump() + " want " + w[k].dump();
			for (int i = 0; i < IM::kReasonCount; i++) {
				const char* id = IM::ReasonId(i);
				if (w["excluded"][id] != json(g.data.excluded[i]))
					diff += std::string(" excluded.") + id + " got " + Num(g.data.excluded[i]) + " want " + w["excluded"][id].dump();
				if (w["samples"][id] != json(g.data.samples[i])) diff += std::string(" samples.") + id;
			}
			if (!diff.empty()) fail(kind, r[1].get<std::string>() + diff);
			line("    " + r[1].get<std::string>() + ": B (Node) built the same data in " + w["ms"].dump() + " ms");
		} else if (kind == "entry") {
			const size_t i = r[2].get<size_t>();
			if (i >= g.data.entries.size() || EJ(g.data.entries[i]) != r[3])
				fail(kind, "#" + Num((long long)i) + " got " +
				               (i < g.data.entries.size() ? Dump(EJ(g.data.entries[i])) : std::string("(none)")) + " want " + r[3].dump());
		} else if (kind == "frags") {
			const Lang lang = LangOf(r[2].get<std::string>());
			std::vector<AlgoValue> vals;
			for (const json& v : r[3]) vals.push_back(ValueFrom(v));
			json out = json::array();
			for (const IM::Entry& e : g.data.entries) {
				json row = json::array();
				for (const AlgoValue& v : vals) {
					const std::optional<std::string> f = IM::Fragment(e.Anchor(lang), v);
					row.push_back(f ? json(*f) : json(nullptr));
				}
				out.push_back(std::move(row));
			}
			const uint32_t h = Fnv1a(Dump(out));
			if (h != r[4].get<uint32_t>()) {
				std::string first;
				for (size_t i = 0; i < r[5].size() && first.empty(); i++)
					if (out[i] != r[5][i]) first = " first diff #" + Num((long long)i) + " got " + Dump(out[i]).substr(0, 300) + " want " + r[5][i].dump().substr(0, 300);
				fail(kind, r[1].get<std::string>() + "/" + r[2].get<std::string>() + " hash differs" + first);
			}
		} else if (kind == "anchor") {
			const Lang lang = LangOf(r[2].get<std::string>());
			std::vector<std::string> same = r[6].get<std::vector<std::string>>();
			const std::optional<IM::ModAnchor> a = IM::ChooseAnchor(lang == Lang::Zh ? *g.zhIdx : *g.enIdx,
			                                                         r[3].get<std::string>(), r[4].get<bool>(), r[5].get<int>(), same);
			const json got = a ? AJ(*a) : json(nullptr);
			if (got != r[7]) fail(kind, r[3].dump() + " limit " + r[5].dump() + " got " + Dump(got) + " want " + r[7].dump());
		} else if (kind == "cats") {
			json cats = json::array();
			for (const IM::StatLite& s : g.zh)
				if (s.hasId) cats.push_back(CatId(IM::CategoryOf(s.ref)));
			if (Fnv1a(Dump(cats)) != r[2].get<uint32_t>()) fail(kind, r[1].get<std::string>() + " hash differs");
		} else if (kind == "groupCounts") {
			if (json(IM::GroupCounts(g.page)) != r[2]) fail(kind, Dump(json(IM::GroupCounts(g.page))) + " want " + r[2].dump());
		} else if (kind == "filter") {
			IM::Filter f;
			f.search = r[2].get<std::string>();
			f.group = r[3].get<int>();
			f.pickedOnly = r[4].get<bool>();
			const IM::Filtered got = IM::FilterRows(g.page, r[5].get<std::vector<int>>(), f, r[6].get<int>());
			if (json(got.rows) != r[7] || got.total != r[8].get<int>())
				fail(kind, r[2].dump() + " g" + r[3].dump() + " got " + Num(got.total) + " " + Dump(json(got.rows)).substr(0, 200) +
				               " want " + r[8].dump() + " " + r[7].dump().substr(0, 200));
		} else if (kind == "combine") {
			const std::string game = r[1].get<std::string>();
			const Lang lang = LangOf(r[2].get<std::string>());
			std::vector<RegexAlgo::ValueMap> values(r[3].size());
			std::vector<RegexAlgo::CombineSel> sels;
			bool ok = true;
			for (size_t k = 0; k < r[3].size(); k++) {
				const json& s = r[3][k];
				const std::string id = s["id"].get<std::string>();
				RegexAlgo::CombineSel sel;
				if (id == g.page.id) {
					sel.page.algo = &g.page;
					for (auto it = s["values"].begin(); it != s["values"].end(); ++it) values[k][it.key()] = ValueFrom(it.value());
					sel.values = &values[k];
				} else {
					const RegexPageDef* c = nullptr;
					for (const RegexPageDef& p : ds.Pages())
						if (p.game == game && p.id == id) c = &p;
					if (!c) { ok = false; break; }
					sel.page.corpus = c;
				}
				sel.picks = s["picks"].get<std::vector<int>>();
				sels.push_back(sel);
			}
			if (!ok) { fail(kind, "unknown page"); continue; }
			const RegexAlgo::CombineResult res = RegexAlgo::Combine(lang, RegexGen::Mode::Any, sels);
			json conflicts = json::array();
			for (const RegexAlgo::Conflict& c : res.conflicts)
				conflicts.push_back(json::array({RegexAlgo::ConflictKindId(c.kind), c.page, c.entry, c.text}));
			json perPage = json::array();
			for (const RegexAlgo::PageContribution& c : res.perPage)
				perPage.push_back({{"id", c.id}, {"picked", c.picked}, {"length", c.length}, {"unresolved", c.unresolved}, {"fragments", c.fragments}});
			const json& w = r[4];
			std::string diff;
			if (res.query != w["query"].get<std::string>()) diff += " query got " + json(res.query).dump() + " want " + w["query"].dump();
			if (res.length != w["length"].get<int>()) diff += " length";
			if (perPage != w["perPage"]) diff += " perPage got " + Dump(perPage).substr(0, 300) + " want " + w["perPage"].dump().substr(0, 300);
			if ((int)res.conflicts.size() != w["conflictsN"].get<int>() || Fnv1a(Dump(conflicts)) != w["conflictsHash"].get<uint32_t>())
				diff += " conflicts got " + Num((long long)res.conflicts.size()) + " " + Dump(conflicts).substr(0, 300) + " want " +
				        w["conflictsN"].dump() + " " + w["conflicts"].dump().substr(0, 300);
			if (!diff.empty()) fail(kind, game + diff);
		} else {
			fail("unknown", kind);
		}
	}
	check(parseErr == 0 && !total.empty(),
	      "R7 golden fixture parses (" + Num((long long)(sizeof kRegexR7Golden / sizeof kRegexR7Golden[0])) + " records)");
	for (const auto& kv : total) {
		const int b = bad[kv.first];
		check(b == 0, "R7 golden " + kv.first + ": " + Num(kv.second - b) + " / " + Num(kv.second) + " identical" +
		                  (b ? "\n      first: " + firstBad[kv.first] : std::string()));
	}
	if (bad.count("unknown")) check(false, "unknown record kind " + firstBad["unknown"]);
	line("    (R7 golden took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}

// ---- item-mods.test.ts (1): uniqueness, full ------------------------------------------

std::string StripPlusTrim(const std::string& s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++)
		if (!(s[i] == '+' && i + 1 < s.size() && s[i + 1] == '#')) o += s[i];
	return RegexAlgo::JsTrim(o);
}
std::string SameKey(const std::string& s, Lang l)
{
	const std::string t = StripPlusTrim(s);
	return l == Lang::En ? IM::JsLower(t) : t;
}
std::vector<std::string> SplitNl(const std::string& s)
{
	std::vector<std::string> out;
	size_t a = 0;
	for (;;) {
		const size_t p = s.find('\n', a);
		out.push_back(s.substr(a, p == std::string::npos ? std::string::npos : p - a));
		if (p == std::string::npos) return out;
		a = p + 1;
	}
}
std::string ReplaceAll(const std::string& s, const std::string& from, const std::string& to)
{
	std::string o;
	size_t a = 0;
	for (size_t p; (p = s.find(from, a)) != std::string::npos; a = p + from.size()) o += s.substr(a, p - a) + to;
	return o + s.substr(a);
}

const char* const kVals[] = {"0", "1", "2", "5", "7", "9", "10", "12", "25", "50", "99", "100", "200", "999", "1000", "12345"};
// item-mods-marker.test.ts MARKERS: a fractured / fixed / rune line goes on with " (" + a tag after its text
// (B 189988e). Every marker starts with " (", which is all the fragment's line end ($| \() looks at.
const char* const kMarker = " (fractured)";

// instances(): every '#' -> v; and /\+?#/g -> '+v'; each also + kMarker (item-mods-marker.test.ts instancesWithMarkers)
void Instances(const std::string& l, std::vector<std::string>& out)
{
	out.clear();
	if (l.find('#') == std::string::npos) {
		out.push_back(l);
	} else {
		for (const char* v : kVals) {
			out.push_back(ReplaceAll(l, "#", v));
			std::string t;
			for (size_t i = 0; i < l.size(); i++) {
				if (l[i] == '+' && i + 1 < l.size() && l[i + 1] == '#') continue;
				if (l[i] == '#') t += std::string("+") + v;
				else t += l[i];
			}
			out.push_back(t);
		}
	}
	const size_t n = out.size();
	for (size_t i = 0; i < n; i++) out.push_back(out[i] + kMarker);
}

// The entry's fragment for any value ({min: 0} = 0 up, no upper bound; item-mods-marker.test.ts T4). Built by
// IM::Fragment itself, so the line end ($| \() and a multi-form alternation are what is checked.
std::optional<std::string> GenericOf(const IM::ModAnchor& a)
{
	AlgoValue any;
	any.min = 0;
	return IM::Fragment(a, any);
}

// filterKey: the longest digit-free stretch of p \u0001 s, lower-cased.
std::string FilterKey(const IM::ModAnchor& a)
{
	const std::string all = a.p + "\x01" + a.s;
	std::string best, cur;
	for (char c : all) {
		if ((c >= '0' && c <= '9') || c == '\x01') {
			if (cur.size() > best.size()) best = cur;   // byte length is a proxy only for which piece; see below
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (cur.size() > best.size()) best = cur;
	return IM::JsLower(best);
}

void UniquenessTests(GameData& g, const std::string& game)
{
	for (Lang l : {Lang::Zh, Lang::En}) {
		const DWORD t0 = GetTickCount();
		const std::vector<IM::StatLite>& st = l == Lang::Zh ? g.zh : g.en;
		std::vector<std::string> lines, lower;
		{
			std::unordered_set<std::string> seen;
			for (const IM::StatLite& s : st)
				for (const std::string& str : s.strings)
					for (const std::string& part : SplitNl(str)) {
						const std::string t = RegexAlgo::JsTrim(part);
						if (!t.empty() && seen.insert(t).second) lines.push_back(t);
					}
			for (const std::string& x : lines) lower.push_back(IM::JsLower(x));
		}
		std::unordered_map<std::string, std::unordered_set<std::string>> same;
		for (const IM::StatLite& s : st) {
			if (!s.hasId) continue;
			auto& cur = same[s.statId + "|" + s.ref];
			for (const std::string& str : s.same)
				for (const std::string& part : SplitNl(str)) cur.insert(SameKey(part, l));
		}
		long long checkedLines = 0, checkedStrings = 0, aborted = 0;
		std::vector<std::string> bad;
		std::vector<std::string> inst;
		RxFlags fl;
		fl.icase = true;
		const std::unordered_set<std::string> empty;
		for (const IM::Entry& e : g.data.entries) {
			const IM::ModAnchor& a = e.Anchor(l);
			const std::optional<std::string> gen = GenericOf(a);
			if (!gen) {
				bad.push_back(u8"沒有片段（{min: 0}）：" + e.ref);
				continue;
			}
			std::string err;
			const std::optional<Rx> re = RxCompile(*gen, &err, fl);
			if (!re) {
				bad.push_back("compile " + *gen + " " + err);
				continue;
			}
			// The key is the longest digit-free piece by UTF-16 length in the TS; any
			// digit-free piece is a valid filter (a line without it cannot match), and
			// the count of lines checked is reported, so the byte-length choice here
			// only changes how much work is skipped, never what is found.
			// With a multi-form alternation p / s hold the first form only: no filter (every line is checked).
			const std::string key = a.alt ? std::string() : FilterKey(a);
			const std::string own = SameKey(l == Lang::Zh ? e.zh : e.en, l);
			const std::string k = e.id.find('|') != std::string::npos ? e.id : e.id + "|" + e.ref;
			auto it = same.find(k);
			const std::unordered_set<std::string>& sameSet = it == same.end() ? empty : it->second;
			for (size_t i = 0; i < lines.size(); i++) {
				if (!key.empty() && lower[i].find(key) == std::string::npos) continue;
				const std::string norm = SameKey(lines[i], l);
				if (norm == own || sameSet.count(norm)) continue;
				checkedLines++;
				Instances(lines[i], inst);
				for (const std::string& s : inst) {
					checkedStrings++;
					const RxStatus st2 = RxSearchEx(*re, s);
					if (st2 == RxStatus::Aborted) aborted++;
					if (st2 != RxStatus::NoMatch) {
						bad.push_back(e.ref + " | " + *gen + " | " + s);
						break;
					}
				}
			}
			std::string self = l == Lang::Zh ? e.zh : e.en;
			const size_t p = self.find('#');
			if (p != std::string::npos) self.replace(p, 1, "12");
			if (!RxSearch(*re, self)) bad.push_back(u8"自己不中：" + e.ref + " | " + *gen);
			if (!RxSearch(*re, self + kMarker)) bad.push_back(u8"自己加標記不中：" + e.ref + " | " + *gen);
		}
		std::string first;
		for (size_t i = 0; i < bad.size() && i < 5; i++) first += "\n      " + bad[i];
		check(bad.empty() && aborted == 0 && checkedLines > 0,
		      u8"R7 唯一性（全量）" + game + (l == Lang::Zh ? " zh" : " en") + u8"：" + Num((long long)g.data.entries.size()) +
		          u8" 條 × 候選行 " + Num(checkedLines) + u8" 行 / " + Num(checkedStrings) + u8" 個實例，" +
		          Num((long long)bad.size()) + u8" 命中" + (aborted ? u8"，" + Num(aborted) + u8" 次比對器中止" : std::string()) +
		          u8"（" + Num((long long)(GetTickCount() - t0)) + " ms）" + first);
	}
}

// ---- item-mods.test.ts (2): own template, value by value -----------------------------

std::string Shown(const std::string& tmpl, long long n, bool withPlus)
{
	std::string t = StripPlusTrim(tmpl);
	const size_t p = t.find('#');
	if (p != std::string::npos) t.replace(p, 1, (withPlus ? "+" : "") + std::to_string(n));
	return t;
}

void ValueTests(GameData& g, const std::string& game)
{
	const DWORD t0 = GetTickCount();
	std::vector<long long> allValues;
	for (int i = 0; i < 1000; i++) allValues.push_back(i);
	for (long long v : {1000LL, 1001LL, 1234LL, 2000LL, 9999LL, 12345LL}) allValues.push_back(v);
	std::vector<std::string> bad;
	long long full = 0, edges = 0, tests = 0;
	RxFlags fl;
	fl.icase = false;   // new RegExp(f) without flags in the TS
	for (size_t idx = 0; idx < g.data.entries.size(); idx++) {
		const IM::Entry& e = g.data.entries[idx];
		const long long m = 1 + (long long)((idx * 37) % 300);
		struct Cond {
			AlgoValue v;
			long long lo, hi;
		};
		Cond conds[3];
		conds[0].v.min = (double)m; conds[0].lo = m; conds[0].hi = LLONG_MAX;
		conds[1].v.max = (double)m; conds[1].lo = LLONG_MIN; conds[1].hi = m;
		const long long top = m + 1 + (long long)((idx * 13) % 120);
		conds[2].v.min = (double)m; conds[2].v.max = (double)top; conds[2].lo = m; conds[2].hi = top;
		for (Lang l : {Lang::Zh, Lang::En}) {
			const int rot = (int)(idx % 3);
			const std::string& tmpl = l == Lang::Zh ? e.zh : e.en;
			for (int ci = 0; ci < 3; ci++) {
				const Cond& c = conds[ci];
				std::vector<long long> values;
				if (ci == rot) {
					values = allValues;
					full++;
				} else {
					for (long long n : {0LL, 1LL, m - 3, m - 2, m - 1, m, m + 1, m + 2, m + 3, 999LL, 1000LL, 12345LL})
						if (n >= 0) values.push_back(n);
					if (c.v.max) for (long long d : {-1LL, 0LL, 1LL}) values.push_back((long long)*c.v.max + d);
					edges++;
				}
				const std::optional<std::string> f = IM::Fragment(e.Anchor(l), c.v);
				if (!f) {
					bad.push_back(u8"沒有片段：" + e.ref);
					continue;
				}
				std::string err;
				const std::optional<Rx> re = RxCompile(*f, &err, fl);
				if (!re) {
					bad.push_back("compile " + *f);
					continue;
				}
				for (long long n : values) {
					for (bool plus : {false, true}) {
						const bool want = n >= c.lo && n <= c.hi;
						// the full 0..999 sweep plain; the edges also with the line-end marker (same verdict)
						for (int mk = 0; mk < (ci == rot ? 1 : 2); mk++) {
							tests++;
							const std::string text = Shown(tmpl, n, plus) + (mk ? kMarker : "");
							if (RxSearch(*re, text) != want) {
								bad.push_back(e.ref + (l == Lang::Zh ? " [zh] " : " [en] ") + *f + u8" 對「" + text + u8"」判斷錯");
								goto next;
							}
						}
					}
				}
			next:;
			}
		}
	}
	std::string first;
	for (size_t i = 0; i < bad.size() && i < 5; i++) first += "\n      " + bad[i];
	check(bad.empty(), u8"R7 自身模板逐值 " + game + u8"：" + Num(full) + u8" 組（詞綴 × 語言）0–999 逐值 + " + Num(edges) +
	                       u8" 組邊界，共 " + Num(tests) + u8" 次比對（" + Num((long long)(GetTickCount() - t0)) + " ms）" + first);
}

// ---- spelled-out checks -----------------------------------------------------------------

void FixedTests(std::map<std::string, GameData>& G, const std::vector<RegexAlgo::PageRef>& poe1Pages)
{
	// item-mods.test.ts:260 COMMON (PoE1, >= m; line end ($| \() since B 189988e): the same strings.
	struct Common {
		const char* ref;
		int m;
		const char* zh;
		const char* en;
	};
	static const Common kCommon[] = {
		{"+# to maximum Life", 80, u8"^\\+?([89][0-9]|[1-9][0-9]{2,}) 最大生命($| \\()", "^\\+?([89][0-9]|[1-9][0-9]{2,}) to maximum Life($| \\()"},
		{"+#% to Fire Resistance", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,})% 火焰抗性($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,})% to Fire Resistance($| \\()"},
		{"+#% to Cold Resistance", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,})% 冰冷抗性($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,})% to Cold Resistance($| \\()"},
		{"+#% to Lightning Resistance", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,})% 閃電抗性($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,})% to Lightning Resistance($| \\()"},
		{"+#% to Chaos Resistance", 20, u8"^\\+?([2-9][0-9]|[1-9][0-9]{2,})% 混沌抗性($| \\()", "^\\+?([2-9][0-9]|[1-9][0-9]{2,})% to Chaos Resistance($| \\()"},
		{"+#% to all Elemental Resistances", 10, u8"^\\+?[1-9][0-9]{1,}% 全部元素抗性($| \\()", "^\\+?[1-9][0-9]{1,}% to all Elemental Resistances($| \\()"},
		{"+# to Strength", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,}) 力量($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,}) to Strength($| \\()"},
		{"+# to Dexterity", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,}) 敏捷($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,}) to Dexterity($| \\()"},
		{"+# to Intelligence", 30, u8"^\\+?([3-9][0-9]|[1-9][0-9]{2,}) 智慧($| \\()", "^\\+?([3-9][0-9]|[1-9][0-9]{2,}) to Intelligence($| \\()"},
		{"#% increased Movement Speed", 25, u8"^增加 \\+?(2[5-9]|[3-9][0-9]|[1-9][0-9]{2,})% 移動速度($| \\()", "^\\+?(2[5-9]|[3-9][0-9]|[1-9][0-9]{2,})% increased Movement Speed($| \\()"},
	};
	GameData& p1 = G["poe1"];
	int found = 0, same = 0;
	std::string firstBad;
	for (const Common& c : kCommon) {
		for (const IM::Entry& e : p1.data.entries) {
			if (e.ref != c.ref) continue;
			found++;
			AlgoValue v;
			v.min = c.m;
			const std::optional<std::string> fz = IM::Fragment(e.anchors[0], v), fe = IM::Fragment(e.anchors[1], v);
			if (fz && fe && *fz == c.zh && *fe == c.en) same++;
			else if (firstBad.empty()) firstBad = std::string(c.ref) + " got " + (fz ? *fz : "-") + " / " + (fe ? *fe : "-");
			break;
		}
	}
	check(found == 10 && same == 10, u8"PoE1 常用 10 條：GGPK 資料找到 " + Num(found) + u8" 條，片段與 exile-appraiser 逐字相同 " +
	                                     Num(same) + " / 10" + (firstBad.empty() ? std::string() : "  " + firstBad));

	// itemModPage: id, no entries before loading, groups = the categories
	const RegexAlgo::AlgoPage empty1 = IM::MakePage("poe1", nullptr), empty2 = IM::MakePage("poe2", nullptr);
	check(empty1.id == "item_mod_values" && empty2.id == "item_mod_values_poe2" && empty1.entries.empty() &&
	          empty1.groups.size() == 9 && empty1.groupsEn.size() == 9 && IM::IsPageId("item_mod_values_poe2") &&
	          !IM::IsPageId("map_mods") && empty1.kind == RegexPageKind::Numeric,
	      u8"itemModPage：頁 id、未載入時沒有項目、9 個分類");
	// conditions that do not hold (item-mods.test.ts:343)
	{
		IM::ModAnchor a;
		a.p = u8"最大生命";
		a.caret = true;
		a.plus = true;
		AlgoValue none, over, inv, neg, geBig;
		over.max = 1000;
		inv.min = 50;
		inv.max = 10;
		neg.max = -1;
		geBig.min = 5000;
		check(!IM::Fragment(a, none) && !IM::Fragment(a, over) && !IM::Fragment(a, inv) && !IM::Fragment(a, neg) &&
		          IM::Fragment(a, geBig).has_value(),
		      u8"條件不成立 = 沒有片段（沒有值、上限 > 999、下限 > 上限、上限 < 0）；≥ 沒有上限");
		IM::ModAnchor bare;
		bare.s = "x";
		AlgoValue le;
		le.max = 5;
		const std::optional<std::string> f = IM::Fragment(bare, le);
		IM::ModAnchor bang;
		bang.p = "!a";
		AlgoValue ge;
		ge.min = 3;
		// plus = false here (the anchor says so): no \+?; >= is open-ended.
		const std::optional<std::string> fb = IM::Fragment(bang, ge);
		check(f && *f == "(^|[^0-9])[0-5]x" && fb == std::optional<std::string>("\\!a([3-9]|[1-9][0-9]{1,})"),
		      u8"沒有前文時 ≤ 補前界 (^|[^0-9])、前文開頭的 ! 跳脫：" + f.value_or("-") + "  " + fb.value_or("-"));
	}
	// ids unique, one '#' each, P + S within the cap (item-mods.test.ts:120)
	for (auto& kv : G) {
		std::set<std::string> ids;
		bool ok = true;
		std::string why;
		for (const IM::Entry& e : kv.second.data.entries) {
			if (!ids.insert(e.id).second) { ok = false; why = "dup id " + e.id; }
			if (std::count(e.zh.begin(), e.zh.end(), '#') != 1 || std::count(e.en.begin(), e.en.end(), '#') != 1) { ok = false; why = "# count " + e.ref; }
			if (e.anchors[0].cost > IM::kMaxAnchorZh || e.anchors[1].cost > IM::kMaxAnchorEn) { ok = false; why = "cost " + e.ref; }
		}
		check(ok, kv.first + u8"：id 唯一、兩語模板恰好一個 #、P + S 不超過上限（40 / 60）" + (ok ? std::string() : "  " + why));
	}
	// bookmark round trip on the item page: the single-page output is identical (item-mods.test.ts:394)
	{
		GameData& g = G["poe1"];
		std::vector<RegexAlgo::PageRef> pages = poe1Pages;
		RegexAlgo::PageRef ip;
		ip.algo = &g.page;
		pages.push_back(ip);
		int rounds = 0, okN = 0;
		uint32_t s = 4242;
		auto rnd = [&](int n) { s = s * 1664525u + 1013904223u; return (int)((s >> 8) % (uint32_t)n); };
		for (int k = 0; k < 24; k++) {
			std::vector<int> picks;
			RegexAlgo::ValueMap values;
			for (int n = 0; n < 1 + rnd(4); n++) {
				const int i = rnd((int)g.page.entries.size());
				if (std::find(picks.begin(), picks.end(), i) == picks.end()) picks.push_back(i);
				AlgoValue v;
				const int kind = rnd(3);
				if (kind != 1) v.min = rnd(200);
				if (kind != 0) v.max = 200 + rnd(700);
				values[g.page.entries[i].def.id] = v;
			}
			std::sort(picks.begin(), picks.end());
			const std::string lang = k % 2 ? "en" : "zh";
			RegexEmbed::PicksMap pm{{g.page.id, picks}};
			RegexEmbed::ValuesMap vm{{g.page.id, values}};
			const std::optional<RegexBookmark> body = RegexEmbed::BookmarkBodyOf(pages, ip, pm, vm, "poe1", "any", lang);
			const std::optional<RegexEmbed::BookmarkApply> ap = body ? RegexEmbed::BookmarkApplyOf(pages, *body) : std::nullopt;
			rounds++;
			// Step 40: the page has a rarity | corruption section now; a bookmark
			// without `num` restores it unticked (the second pick entry, empty).
			if (!ap || ap->page != g.page.id || ap->missed != 0 || ap->picks.empty() || ap->picks[0].second != picks) continue;
			if (ap->picks.size() != 2 || ap->picks[1].first != "item_mod_values_cond" || !ap->picks[1].second.empty()) continue;
			RegexAlgo::ValueMap back;
			for (const auto& v : ap->values)
				if (v.first == g.page.id)
					for (const auto& kv : v.second) back[kv.first] = kv.second;
			const Lang L = LangOf(lang);
			RegexAlgo::CombineSel a, b;
			a.page = ip;
			a.picks = picks;
			a.values = &values;
			b.page = ip;
			b.picks = ap->picks[0].second;
			b.values = &back;
			const std::string qa = RegexAlgo::CombineSingle(L, RegexGen::Mode::Any, {a}).query;
			const std::string qb = RegexAlgo::CombineSingle(L, RegexGen::Mode::Any, {b}).query;
			if (!qa.empty() && qa == qb && body->keys.size() == picks.size()) okN++;
		}
		check(okN == rounds, u8"書籤往返（物品詞綴數值頁）：" + Num(okN) + " / " + Num(rounds) + u8" 組單頁輸出逐字相同，鍵 = 交易站 stat id");
	}
}

} // namespace

void RegexR7Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	line("--- R7: item-mod values page (regex_itemmods) ---");
	std::map<std::string, GameData> G;
	for (const char* game : {"poe1", "poe2"}) {
		GameData& g = G[game];
		const size_t m0 = PrivateBytes();
		LARGE_INTEGER f, a, b;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&a);
		std::string err;
		g.ok = IM::LoadFile(exeDir, game, g.data, &err);
		QueryPerformanceCounter(&b);
		const size_t m1 = PrivateBytes();
		const long long ms = (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart;
		check(g.ok && !g.data.entries.empty(), std::string(u8"載入 Data\\regex_stats\\") + game + u8"（stats.ndjson.gz 兩語，同面板的延遲載入）：" +
		                                           Num((long long)g.data.entries.size()) + u8" 條可選 / 物品詞綴 " + Num(g.data.itemStats) +
		                                           u8"，讀檔 + 解析 + 建索引 + 選錨點 " + Num(ms) + u8" ms，留存記憶體約 " +
		                                           Num((long long)((m1 > m0 ? m1 - m0 : 0) / 1024)) + " KB" + (g.ok ? "" : "  " + err));
		if (!g.ok) continue;
		std::string ex;
		for (int i = 0; i < IM::kReasonCount; i++) ex += std::string(i ? ", " : "") + IM::ReasonId(i) + " " + Num(g.data.excluded[i]);
		line(std::string("    ") + game + u8"：同字併列 " + Num(g.data.merged) + u8"；排除 " + ex);
		std::string perr;
		IM::LoadStats(exeDir, game, g.zh, g.en, &perr);
		g.zhIdx = IM::BuildIndex(g.zh, Lang::Zh);
		g.enIdx = IM::BuildIndex(g.en, Lang::En);
		line(std::string("    ") + game + u8" 語料：繁中 " + Num((long long)IM::IndexLineCount(*g.zhIdx)) + u8" 行 / " +
		     Num((long long)IM::IndexRunCount(*g.zhIdx)) + u8" 個數字位置，英文 " + Num((long long)IM::IndexLineCount(*g.enIdx)) + u8" 行 / " +
		     Num((long long)IM::IndexRunCount(*g.enIdx)) + u8" 個數字位置");
		g.page = IM::MakePage(game, &g.data);
	}
	RegexDataset ds;
	std::string derr;
	ds.Load(exeDir, L"poe1", &derr);
	GoldenTests(G, ds);
	for (auto& kv : G)
		if (kv.second.ok) {
			UniquenessTests(kv.second, kv.first);
			ValueTests(kv.second, kv.first);
		}
	std::vector<RegexAlgo::PageRef> poe1Pages;
	std::vector<RegexAlgo::AlgoPage> algo = RegexAlgo::AlgoPages("poe1", ds.Labels("poe1"));
	for (const RegexPageDef& p : ds.Pages())
		if (p.game == "poe1") poe1Pages.push_back({&p, nullptr});
	for (const RegexAlgo::AlgoPage& p : algo) poe1Pages.push_back({nullptr, &p});
	if (G["poe1"].ok) FixedTests(G, poe1Pages);
}
