// Fragment builders for the algorithmic regex pages (numeric sections, vendor
// items): property lines, map tiers, rarity, sockets. Ported from exile-appraiser
// `regex/src/pages/frag.ts` (+ `rangeOp` from pages/types.ts); each function
// names the TS file:line it mirrors, and the golden fixture in regex_selftest.cpp
// holds them to byte-identical output.
//
// Pure functions: no ImGui, no files. std::nullopt is the TS `null` ("the input
// does not describe anything", e.g. an empty range).
//
// The fragments only use `.` `*` `[^\d]` `\d` `[...]` `?` `|` `()` `^` `$`, plus
// `\+` `\)` `{n,}` `[:：]` in the strict forms; regex_match.h understands all of
// it, which is how the self-test checks them value by value.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace RegexFrag {

// pages/types.ts:17 AlgoValue. range: min / max (missing = unbounded); select /
// colors / count: choice.
struct AlgoValue {
	std::optional<double> min;
	std::optional<double> max;
	std::string choice;
	// TS tells `choice: ''` apart from no choice at all (view.ts condText returns
	// '' for the first and null for the second). A non-empty `choice` is present
	// either way; this flag only marks an explicitly EMPTY one. See HasChoice.
	bool hasChoice = false;
};

// Is `choice` defined (TS `v.choice !== undefined`)?
inline bool HasChoice(const AlgoValue& v) { return v.hasChoice || !v.choice.empty(); }

// pages/types.ts:11 RangeOp; None = TS null.
enum class RangeOp { None, Ge, Le, Range };

// pages/types.ts:65 rangeOp: >= / <= / range from which bounds are finite numbers.
RangeOp RangeOpOf(const AlgoValue& v);

// frag.ts:8 labelBase: drop '#', trailing whitespace / ':' / '：', then trim.
// ("物品等級：#" -> "物品等級", "Item Level #" -> "Item Level")
std::string LabelBase(const std::string& label);

// frag.ts:21 propertyFragment: "label .* value" with the boundary rules the TS
// comment explains (vendor page).
std::optional<std::string> PropertyFragment(const std::string& label, const AlgoValue& v,
                                            int digits, bool percent, bool anchorStart = false);

// frag.ts:47 strictPropertyFragment (map / waystone numeric section). R10: the
// percent form is "label: \+?<n>%", the way the game prints the line
// ("怪群大小: +13% (augmented)"); the non-percent form is unchanged.
std::optional<std::string> StrictPropertyFragment(const std::string& label, const AlgoValue& v,
                                                  int digits, bool percent);

enum class Lang { Zh, En };

// frag.ts:72 TIER_NAME_FORMAT + frag.ts:78 isTierNameLine: is this corpus line
// (with '#') a "名稱（階級 #）" / "Name (Tier #)" item name?
bool IsTierNameLine(const std::string& line);

// frag.ts:82 mapTierFragment: the tier sits at the end of the item NAME
// ("地圖（階級 16）", "Map (Tier 16)"), not on a property line. R10: `hi` > 0 =
// the highest tier there is; a >= condition then becomes the range min..hi.
std::optional<std::string> MapTierFragment(const AlgoValue& v, int digits, Lang lang, int hi = 0);

// frag.ts:92 rarityFragment: "稀有度[:：] *稀有"
std::optional<std::string> RarityFragment(const std::string& label, const std::string& value);

// frag.ts:100 wholeLine: ^label$ / ^(a|b)$ (de-duplicated, order kept)
std::optional<std::string> WholeLine(const std::vector<std::string>& labels);

// frag.ts:107 linkedSockets: n linked sockets ".-.-." (2..6)
std::optional<std::string> LinkedSockets(int n);

// frag.ts:155 linkColors: linked sockets holding these colours in any order,
// e.g. "rgb" -> "b-(g-r|r-g)|g-(b-r|r-b)|r-(b-g|g-b)".
std::optional<std::string> LinkColors(const std::string& choice);

// frag.ts:172 socketColorCount: "插槽.*b.*b.*b"
std::optional<std::string> SocketColorCount(const std::string& label, const std::string& color, int n);

} // namespace RegexFrag
