#include "filter_tierlist.h"
#include "filter_parser.h"

#include <cstddef>
#include <algorithm>
#include <unordered_map>

namespace {

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

bool starts_with(const std::string& s, const char* prefix)
{
	size_t n = 0;
	while (prefix[n]) n++;
	return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Split "a->b->c" into {"a","b","c"}.
std::vector<std::string> split_arrow(const std::string& s)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t e = s.find("->", pos);
		if (e == std::string::npos) { out.push_back(s.substr(pos)); break; }
		out.push_back(s.substr(pos, e - pos));
		pos = e + 2;
	}
	return out;
}

// Find the first BaseType condition line in a block; -1 if none.
int find_basetype_line(const FilterFile& f, const FilterBlock& b)
{
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind == FilterLineKind::Condition && ln.keyword == "BaseType")
			return li;
	}
	return -1;
}

// Friendly big-category for a NeverSink $type first segment. Collapses the ~49
// raw segments into a handful of human-meaningful groups with a fixed display
// order. Unmapped segments fall into "other" (nothing is dropped).
struct FriendlyCat { const char* key; const char* zh; int order; };

static FriendlyCat friendly_category(const std::string& seg)
{
	static const FriendlyCat kCurrency  = { "currency",   u8"通貨",     1 };
	static const FriendlyCat kFragments = { "fragments",  u8"碎片",     2 };
	static const FriendlyCat kDivers    = { "divination", u8"命運卡",   3 };
	static const FriendlyCat kMaps      = { "maps",       u8"地圖",     4 };
	static const FriendlyCat kUniques   = { "uniques",    u8"傳奇",     5 };
	static const FriendlyCat kGems      = { "gems",       u8"寶石",     6 };
	static const FriendlyCat kJewels    = { "jewels",     u8"珠寶",     7 };
	static const FriendlyCat kGear      = { "gear",       u8"裝備武器", 8 };
	static const FriendlyCat kFlasks    = { "flasks",     u8"藥劑",     9 };
	static const FriendlyCat kOther     = { "other",      u8"其他",    10 };

	static const std::unordered_map<std::string, const FriendlyCat*> kMap = {
		{ "currency", &kCurrency }, { "gold", &kCurrency }, { "vials", &kCurrency }, { "chancing", &kCurrency },
		{ "fragments", &kFragments },
		{ "divination", &kDivers },
		{ "maps", &kMaps }, { "exoticmap", &kMaps }, { "maphiders", &kMaps }, { "miscmapitems", &kMaps }, { "miscmapitemsextra", &kMaps },
		{ "uniques", &kUniques },
		{ "gems", &kGems },
		{ "jewels", &kJewels },
		{ "endgameflasks", &kFlasks }, { "endgametinctures", &kFlasks },
		// gear-related rule categories
		{ "leveling", &kGear }, { "rareid", &kGear }, { "rareblendid", &kGear }, { "rareoptional", &kGear },
		{ "crafting", &kGear }, { "normalcraft", &kGear }, { "exoticmods", &kGear }, { "exotic", &kGear },
		{ "exoticbases", &kGear }, { "exoticbaseslower", &kGear }, { "decorators", &kGear }, { "magicid", &kGear },
		{ "rare", &kGear }, { "influenced", &kGear }, { "gear", &kGear }, { "artefact", &kGear },
		{ "socketslinks", &kGear }, { "6l", &kGear }, { "corruptedid", &kGear }, { "endgamergb", &kGear },
	};
	auto it = kMap.find(seg);
	return it != kMap.end() ? *it->second : kOther;
}

// A short label when a block has no $tier-> name (rare; all NeverSink blocks do).
std::string fallback_label(const FilterFile& f, const FilterBlock& b)
{
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind == FilterLineKind::Condition) {
			std::string s = ln.keyword;
			if (!ln.values.empty()) { s += ' '; s += ln.values[0].text; }
			return s;
		}
	}
	return b.hide ? "(Hide)" : "(Show)";
}

} // namespace

TierMarkers ParseTierMarkers(const std::string& headerComment)
{
	TierMarkers m;
	const std::string& s = headerComment;
	size_t i = 0, n = s.size();
	while (i < n) {
		while (i < n && is_space(s[i])) i++;
		if (i >= n) break;
		size_t start = i;
		while (i < n && !is_space(s[i])) i++;
		std::string tok = s.substr(start, i - start);

		if (!tok.empty() && tok[0] == '%') {
			if (m.styleCode.empty()) m.styleCode = tok.substr(1); // keep the first
		} else if (starts_with(tok, "$type->")) {
			m.typePath = split_arrow(tok.substr(7));
		} else if (starts_with(tok, "$tier->")) {
			m.tierName = tok.substr(7);
		}
		// any other token ($artefactex, FilterBlade extras, ...) is ignored
	}
	return m;
}

TierListIndex BuildTierList(const FilterFile& f)
{
	TierListIndex idx;
	idx.tiers.reserve(f.blocks.size());

	for (int bi = 0; bi < (int)f.blocks.size(); bi++) {
		const FilterBlock& b = f.blocks[bi];
		TierEntry e;
		e.blockIndex = bi;
		e.hide = b.hide;
		e.markers = ParseTierMarkers(b.headerComment);
		e.tierLabel = e.markers.tierName.empty() ? fallback_label(f, b) : e.markers.tierName;

		e.baseTypeLineIdx = find_basetype_line(f, b);
		if (e.baseTypeLineIdx >= 0) {
			const FilterLine& ln = f.lines[e.baseTypeLineIdx];
			e.baseTypeUsesEqEq = (ln.op == "==");
			e.items.reserve(ln.values.size());
			for (const FilterToken& t : ln.values) e.items.push_back(t.text);
		}
		idx.tiers.push_back(std::move(e));
	}

	// Group tiers into friendly big-categories (file order within a category).
	for (int ti = 0; ti < (int)idx.tiers.size(); ti++) {
		const TierEntry& e = idx.tiers[ti];
		std::string seg = e.markers.hasType() ? e.markers.typePath[0] : std::string();
		FriendlyCat fc = friendly_category(seg);

		int catIdx = -1;
		for (int c = 0; c < (int)idx.categories.size(); c++)
			if (idx.categories[c].key == fc.key) { catIdx = c; break; }
		if (catIdx < 0) {
			TierCategory cat;
			cat.key = fc.key;
			cat.display = fc.zh;
			cat.order = fc.order;
			idx.categories.push_back(std::move(cat));
			catIdx = (int)idx.categories.size() - 1;
		}
		TierCategory& cat = idx.categories[catIdx];
		cat.tierEntryIdx.push_back(ti);
		cat.totalItems += (int)e.items.size();
		if (e.draggable()) cat.draggableTiers++;
	}

	// Stable sort categories by their fixed display order (tierEntryIdx stays file order).
	std::stable_sort(idx.categories.begin(), idx.categories.end(),
		[](const TierCategory& a, const TierCategory& b) { return a.order < b.order; });

	return idx;
}

TierMoveResult MoveItemToTier(FilterFile& f, int srcBlockIndex, int dstBlockIndex,
                              const std::string& itemText)
{
	if (srcBlockIndex == dstBlockIndex) return TierMoveResult::SameTier;
	if (srcBlockIndex < 0 || srcBlockIndex >= (int)f.blocks.size() ||
		dstBlockIndex < 0 || dstBlockIndex >= (int)f.blocks.size())
		return TierMoveResult::ItemMissing;

	int srcLine = find_basetype_line(f, f.blocks[srcBlockIndex]);
	int dstLine = find_basetype_line(f, f.blocks[dstBlockIndex]);
	if (dstLine < 0) return TierMoveResult::DstNotDraggable;
	if (srcLine < 0 || !FilterHasValue(f.lines[srcLine], itemText)) return TierMoveResult::ItemMissing;
	if (FilterHasValue(f.lines[dstLine], itemText)) return TierMoveResult::Duplicate;

	// Preserve the source token's quoting; always quote if it contains a space.
	bool quoted = itemText.find(' ') != std::string::npos;
	for (const FilterToken& t : f.lines[srcLine].values)
		if (t.text == itemText) { quoted = quoted || t.quoted; break; }

	FilterAddValue(f.lines[dstLine], itemText, quoted);
	FilterRemoveValue(f.lines[srcLine], itemText);
	f.dirty = true;
	return TierMoveResult::Ok;
}
