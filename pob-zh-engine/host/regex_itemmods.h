// The item-mod VALUE page of the Poe Regex tool (R7): one row per single-number
// item modifier ("+# to maximum Life"), each ticked row its own term
// "^\+?<number range> 最大生命$". Ported from exile-appraiser
// `regex/src/pages/item-mods.ts` (step 37); every function names the TS line it
// mirrors and regex_r7_golden.inc (that TS run over OUR data file) holds them to
// identical output.
//
// Data: NOT the trade site's stats.ndjson the TS reads -- the project's rule is
// "GGPK first", so Data\regex_itemmods_<game>.json is produced from the GGPK by
// the local tools/gen_regex_itemmods.py: one record per stat group (key = the
// GGPK stat id, `ref` = the English template; lines tagged plain / negated /
// fixed) plus `otherZh/otherEn`, every other line of the description files those
// items print through (the uniqueness corpus). Parsed here into the same
// StatLite shape item-mods.ts builds from stats.ndjson, then the same
// buildItemModData. Consequence: this page's entry ids are GGPK stat ids, so its
// share-code keys do not match exile-appraiser's (every other page's do).
//
// String semantics follow JavaScript: the algorithm runs on UTF-16 code units
// (lengths, slices, the sort order, the 40 / 60 length caps), English is folded
// with a JS-compatible toLowerCase, and only the results go back to UTF-8.
//
// Pure apart from LoadFile: no ImGui. The panel builds it on a worker thread the
// first time the page is opened (store.ts ensureItemMods).
#pragma once

#include "regex_algo_pages.h"
#include "regex_frag.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RegexItemMods {

using RegexFrag::AlgoValue;
using RegexFrag::Lang;

// item-mods.ts:34 ITEM_MOD_PAGE_IDS / :39 isItemModPageId
const char* PageId(const std::string& game);
bool IsPageId(const std::string& id);

// item-mods.ts:50 MAX_ANCHOR_TEXT (zh 40, en 60) / :415 ITEM_MOD_MAX
constexpr int kMaxAnchorZh = 40;
constexpr int kMaxAnchorEn = 60;
constexpr int kItemModMax = 999;

// item-mods.ts:54 ItemModCategory, in ITEM_MOD_CATEGORIES order (:56).
enum class Category { Life, Mana, Es, Resist, Attr, Speed, Damage, Move, Other };
constexpr int kCategoryCount = 9;
const char* CategoryZh(int i);
const char* CategoryEn(int i);
// item-mods.ts:75 itemModCategory: English ref keywords, first match wins.
Category CategoryOf(const std::string& ref);

// item-mods.ts:90 StatLite (UTF-8). `hasId` false = TS statId null (corpus only).
struct StatLite {
	std::string ref;
	std::string statId;
	bool hasId = false;
	bool dp = false;
	std::vector<std::string> strings;   // every line (plain, negated, fixed)
	std::vector<std::string> plain;     // non-negated, non-fixed
	std::vector<std::string> same;      // non-negated (fixed included)
};

// Data\regex_itemmods_<game>.json -> the two languages' StatLite lists
// (tools/regex_port/r7-adapter.ts aStats, the exact same mapping).
bool ParseFile(const std::string& body, std::vector<StatLite>& zh, std::vector<StatLite>& en,
               std::string* err);

// item-mods.ts:401 ModAnchor (p / s in UTF-8; cost in UTF-16 units)
struct ModAnchor {
	std::string p, s;
	bool caret = false, dollar = false, plus = false;
	int cost = 0;
};

// item-mods.ts:394 escapeFragText
std::string EscapeFragText(const std::string& s);

// item-mods.ts:418 itemModFragment: nullopt = the condition does not hold.
std::optional<std::string> Fragment(const ModAnchor& a, const AlgoValue& v);

// item-mods.ts:156 / :184 buildModIndex, :336 chooseAnchor. The index is kept
// for --regex-selftest; the panel only needs the result of BuildData.
struct ModIndex;
struct IndexDeleter { void operator()(ModIndex* p) const; };
using ModIndexPtr = std::unique_ptr<ModIndex, IndexDeleter>;
ModIndexPtr BuildIndex(const std::vector<StatLite>& stats, Lang lang);
std::optional<ModAnchor> ChooseAnchor(const ModIndex& idx, const std::string& tmpl, bool plusHint, int limit,
                                      const std::vector<std::string>& sameLines);
size_t IndexLineCount(const ModIndex& idx);
size_t IndexRunCount(const ModIndex& idx);

// item-mods.ts:442 ItemModExcludeReason, in the TS object's key order.
enum class Reason { Decimal, MultiValue, MultiForm, Multiline, MissingLang, NoUnique, TooLong };
constexpr int kReasonCount = 7;
const char* ReasonId(int r);   // "decimal" ...

// item-mods.ts:459 ItemModEntryData
struct Entry {
	std::string id, ref, zh, en;
	Category cat = Category::Other;
	bool percent = false;
	ModAnchor anchors[2];   // [0] zh, [1] en
	const ModAnchor& Anchor(Lang l) const { return anchors[l == Lang::Zh ? 0 : 1]; }
};

// item-mods.ts:446 ItemModData
struct Data {
	std::string game;
	std::vector<Entry> entries;
	int itemStats = 0;
	int merged = 0;
	std::array<int, kReasonCount> excluded{};
	std::array<std::vector<std::string>, kReasonCount> samples;   // up to 8 English refs each
};

// item-mods.ts:497 buildItemModData
Data BuildData(const std::string& game, const std::vector<StatLite>& zh, const std::vector<StatLite>& en);

// Read + parse + build Data\regex_itemmods_<game>.json. False with *err on a
// missing / broken file.
bool LoadFile(const std::wstring& exeDir, const std::string& game, Data& out, std::string* err);

// item-mods.ts:555 itemModPage: data null = the page without entries (listed,
// nothing to tick until it is loaded).
RegexAlgo::AlgoPage MakePage(const std::string& game, const Data* data);

// item-mods.ts:585 ItemModFilter / :601 filterItemMods / :624 itemModGroupCounts
struct Filter {
	std::string search;
	int group = -1;
	bool pickedOnly = false;
};
struct Filtered {
	std::vector<int> rows;
	int total = 0;
};
Filtered FilterRows(const RegexAlgo::AlgoPage& page, const std::vector<int>& picked, const Filter& f, int cap = 200);
std::vector<int> GroupCounts(const RegexAlgo::AlgoPage& page);

// The panel's row cap (RegexItemModList.vue CAP).
constexpr int kListCap = 150;

// JS String.prototype.toLowerCase on UTF-8 (BMP case mappings; see the .cpp).
std::string JsLower(const std::string& s);

} // namespace RegexItemMods
