#include "regex_embed.h"

#include <algorithm>
#include <set>

namespace RegexEmbed {

using RegexAlgo::AlgoEntry;
using RegexAlgo::AlgoPage;
using RegexFrag::AlgoValue;

const std::string& KeyOf(const RegexEntryDef& e)
{
	static const std::string empty;
	return e.en.empty() ? empty : e.en[0];
}

const std::string& ZhLine(const RegexEntryDef& e)
{
	static const std::string empty;
	if (!e.zh.empty()) return e.zh[0];
	if (!e.en.empty()) return e.en[0];
	return empty;
}

Keys PageKeysOf(const PageRef& page, const std::vector<int>& picked)
{
	Keys out;
	const std::set<int> set(picked.begin(), picked.end());
	const size_t n = page.Size();
	for (size_t i = 0; i < n; i++) {
		if (!set.count((int)i)) continue;
		if (page.corpus) {
			out.keys.push_back(KeyOf(page.corpus->entries[i]));
			out.alt.push_back(ZhLine(page.corpus->entries[i]));
		} else {
			out.keys.push_back(page.algo->entries[i].def.id);
			out.alt.push_back(ZhLine(page.algo->entries[i].def));
		}
	}
	return out;
}

Applied ApplyPageKeys(const PageRef& page, const std::vector<std::string>& keys,
                      const std::vector<std::string>& alt)
{
	Applied out;
	if (page.corpus) {
		std::vector<std::string> ek, ea;
		for (const RegexEntryDef& d : page.corpus->entries) {
			ek.push_back(KeyOf(d));
			ea.push_back(ZhLine(d));
		}
		std::vector<char> picked;
		out.missed = RegexResolveKeys(keys, alt, ek, ea, picked);
		for (int i = 0; i < (int)picked.size(); i++)
			if (picked[i]) out.picked.push_back(i);
		return out;
	}
	std::set<int> picked;
	for (const std::string& k : keys) {
		int hit = -1;
		for (int i = 0; i < (int)page.algo->entries.size() && hit < 0; i++)
			if (page.algo->entries[i].def.id == k) hit = i;
		if (hit >= 0) picked.insert(hit);
		else out.missed++;
	}
	out.picked.assign(picked.begin(), picked.end());
	return out;
}

std::optional<Applied> SavedPicksOf(const PageRef& page, const RegexUiState& s)
{
	const std::string hostId = page.IsSection() ? page.algo->sectionOf : page.Id();
	for (const RegexPagePicks& p : s.current) {
		if (p.page != hostId) continue;
		return page.IsSection() ? ApplyPageKeys(page, p.num) : ApplyPageKeys(page, p.keys, p.alt);
	}
	return std::nullopt;
}

namespace {

const std::vector<int>& PicksIn(const PicksMap& m, const std::string& id)
{
	static const std::vector<int> none;
	auto it = m.find(id);
	return it == m.end() ? none : it->second;
}

const AlgoPage* FindSection(const std::vector<PageRef>& pages, const std::string& hostId)
{
	for (const PageRef& p : pages)
		if (p.IsSection() && p.algo->sectionOf == hostId) return p.algo;
	return nullptr;
}

// Values of the ticked rows: the stored value, else the entry's default.
RegexValueList TickedValues(const AlgoPage& page, const std::vector<int>& picked, const ValuesMap& values,
                            const std::string& key)
{
	RegexValueList m;
	auto cur = values.find(key);
	std::vector<int> sorted = picked;   // TS iterates `own` in tick order; ticks are kept ascending
	for (int i : sorted) {
		if (i < 0 || i >= (int)page.entries.size()) continue;
		const AlgoEntry& e = page.entries[i];
		const AlgoValue* v = nullptr;
		if (cur != values.end()) {
			auto f = cur->second.find(e.def.id);
			if (f != cur->second.end()) v = &f->second;
		}
		RegexValueSet(m, e.def.id, v ? *v : e.input.def);
	}
	return m;
}

} // namespace

std::optional<RegexBookmark> BookmarkBodyOf(const std::vector<PageRef>& pages, const PageRef& page,
                                            const PicksMap& picks, const ValuesMap& values,
                                            const std::string& game, const std::string& mode,
                                            const std::string& lang)
{
	RegexBookmark body;
	body.page = page.Id();
	body.game = game;
	body.mode = mode;
	body.lang = lang;
	const std::vector<int>& own = PicksIn(picks, page.Id());
	Keys k = PageKeysOf(page, own);
	body.keys = std::move(k.keys);
	body.alt = std::move(k.alt);
	if (page.algo && !own.empty())
		body.numeric = TickedValues(*page.algo, own, values, RegexAlgo::NumericKeyOf(page.Id()));
	if (page.corpus) {
		if (const AlgoPage* sec = FindSection(pages, page.Id())) {
			const std::vector<int>& sp = PicksIn(picks, sec->id);
			if (!sp.empty()) {
				PageRef sr;
				sr.algo = sec;
				body.num = PageKeysOf(sr, sp).keys;
				body.numeric = TickedValues(*sec, sp, values, RegexAlgo::NumericKeyOf(sec->id));
			}
		}
	}
	if (body.keys.empty() && body.num.empty()) return std::nullopt;
	return body;
}

std::optional<BookmarkApply> BookmarkApplyOf(const std::vector<PageRef>& pages, const RegexBookmark& b)
{
	const PageRef* target = nullptr;
	for (const PageRef& p : pages)
		if (p.Id() == b.page) {
			target = &p;
			break;
		}
	if (!target) return std::nullopt;
	const PageRef* page = target;
	std::vector<std::string> keys = b.keys, alt = b.alt, num = b.num;
	// Defensive: an unmigrated numeric-page bookmark (Parse already converts them).
	if (target->IsSection()) {
		const PageRef* host = nullptr;
		for (const PageRef& p : pages)
			if (p.Id() == target->algo->sectionOf) host = &p;
		if (!host) return std::nullopt;
		num = b.keys;
		keys.clear();
		alt.clear();
		page = host;
	}
	BookmarkApply out;
	out.page = page->Id();
	const Applied r = ApplyPageKeys(*page, keys, alt);
	out.picks.emplace_back(page->Id(), r.picked);
	out.missed += r.missed;
	const AlgoPage* sec = page->corpus ? FindSection(pages, page->Id()) : nullptr;
	if (sec) {
		// A host bookmark is the whole page: one without `num` (saved before the
		// section existed) restores the section unticked.
		PageRef sr;
		sr.algo = sec;
		const Applied rs = ApplyPageKeys(sr, num);
		out.picks.emplace_back(sec->id, rs.picked);
		out.missed += rs.missed;
		if (!b.numeric.empty() && !num.empty())
			out.values.emplace_back(RegexAlgo::NumericKeyOf(sec->id), b.numeric);
	} else if (!b.numeric.empty() && page->algo) {
		out.values.emplace_back(page->Id(), b.numeric);
	}
	return out;
}

} // namespace RegexEmbed
