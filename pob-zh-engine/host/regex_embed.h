// State <-> ticks for the Poe Regex tool: what a page's saved record, a bookmark
// and a bookmark's restore look like. Ported from exile-appraiser
// regex/src/embed.ts (savedPicksOf / bookmarkBodyOf / bookmarkApplyOf) and
// pages/index.ts (pageKeysOf / applyPageKeys); --regex-selftest holds them to
// that TS's output (regex_r5_golden.inc).
//
// A bookmark is the whole page: a host page (map_mods / waystone_mods) carries
// its numeric section's ticks (`num`) and values (`numeric`); the vendor page
// carries its ticked ids and their values. Loading one overwrites that page
// (and its section) and nothing else.
//
// `pages` is ONE game's catalogue (corpus + algorithmic, sections included),
// because page ids repeat across games (gem_names, vendor_bases). Pure: no
// ImGui, no files.
#pragma once

#include "regex_algo_pages.h"
#include "regex_state.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace RegexEmbed {

using RegexAlgo::PageRef;

struct Keys {
	std::vector<std::string> keys, alt;
};
struct Applied {
	std::vector<int> picked;   // ascending
	int missed = 0;
};

// pages/index.ts pageKeysOf: corpus = English line + Chinese line; algorithmic = entry id + Chinese.
Keys PageKeysOf(const PageRef& page, const std::vector<int>& picked);
// pages/index.ts applyPageKeys
Applied ApplyPageKeys(const PageRef& page, const std::vector<std::string>& keys,
                      const std::vector<std::string>& alt = {});

// embed.ts savedPicksOf: this page's saved ticks (a section's live in its host's `num`).
std::optional<Applied> SavedPicksOf(const PageRef& page, const RegexUiState& s);

using PicksMap = std::map<std::string, std::vector<int>>;            // page id -> ticks
using ValuesMap = std::map<std::string, RegexAlgo::ValueMap>;        // store key -> values

// embed.ts bookmarkBodyOf: the page's current contents as a bookmark (name
// left empty); nullopt = neither the page nor its section has a tick.
std::optional<RegexBookmark> BookmarkBodyOf(const std::vector<PageRef>& pages, const PageRef& page,
                                            const PicksMap& picks, const ValuesMap& values,
                                            const std::string& game, const std::string& mode,
                                            const std::string& lang);

// embed.ts BookmarkApply / bookmarkApplyOf
struct BookmarkApply {
	std::string page;                                          // the list page (a section bookmark -> its host)
	std::vector<std::pair<std::string, std::vector<int>>> picks;   // page id -> ticks to overwrite
	std::vector<std::pair<std::string, RegexValueList>> values;    // store key -> values to merge in
	int missed = 0;
};
// nullopt = the bookmark's page is not in `pages`.
std::optional<BookmarkApply> BookmarkApplyOf(const std::vector<PageRef>& pages, const RegexBookmark& b);

// The line a corpus row is keyed by (English, first line) and its Chinese fallback.
const std::string& KeyOf(const RegexEntryDef& e);
const std::string& ZhLine(const RegexEntryDef& e);

} // namespace RegexEmbed
