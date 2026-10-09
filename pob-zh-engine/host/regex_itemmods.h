// The item-mod VALUE page of the Poe Regex tool (R7): one row per single-number
// item modifier ("+# to maximum Life"), each ticked row its own term
// "^\+?<number range> 最大生命$". Ported from exile-appraiser
// `regex/src/pages/item-mods.ts` (step 37); every function names the TS line it
// mirrors and regex_r7_golden.inc (that TS run over the SAME data) holds them to
// identical output.
//
// Data (2026-10-09, user decision: share codes must interoperate on every page):
// the very stats.ndjson exile-appraiser reads (data/<game>/{cmn-Hant,en}/, itself
// a byte-for-byte copy of APT / EE2's stat table), shipped gzip-compressed as
// Data\regex_stats\<game>\<lang>\stats.ndjson.gz (host/data/regex_stats/
// MANIFEST.json records the source commit and the SHA-256 of the uncompressed
// bytes). Entry ids are therefore trade-site stat ids ("stat_3299347043", or
// "stat_…|<ref>" when two entries share one), the same keys exile-appraiser's
// share codes and bookmarks carry. Until this change the page read a GGPK-made
// Data\regex_itemmods_<game>.json keyed by GGPK stat ids ("base_maximum_life");
// such keys no longer resolve and are reported (IsLegacyKey), never dropped.
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
#include <unordered_map>
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

// item-mods.ts:129 parseStatsNdjson (groups flattened, broken lines skipped).
std::vector<StatLite> ParseStatsNdjson(const std::string& text);

// Inflated stats.ndjson larger than this is refused (the real files are 0.9-2.6 MB).
constexpr size_t kMaxStatsBytes = 64u << 20;

// Data\regex_stats\<game>\<cmn-Hant|en>\stats.ndjson.gz, inflated. False + *err
// ("找不到 stats 資料（…）" / "stats 資料損毀（…）") when missing or broken.
bool LoadStatsText(const std::wstring& exeDir, const std::string& game, Lang lang, std::string& text,
                   std::string* err);
// Both languages, parsed (node.ts loadItemModData's two reads).
bool LoadStats(const std::wstring& exeDir, const std::string& game, std::vector<StatLite>& zh,
               std::vector<StatLite>& en, std::string* err);

// A key this page wrote before it switched to trade stat ids: a GGPK stat id
// such as "base_maximum_life" -- anything not shaped like a trade id (see .cpp).
bool IsLegacyKey(const std::string& key);

// item-mods.ts ModAnchor.alt (B 00bb297): the multi-form arbitration's
// alternation. `side` ('p' or 's') names the part whose UTF-16 units [at, at+len)
// (= the first form's differing run, opts[0]) are replaced by the group
// `(opts...)` / `(X)?`. Absent = a single form (the output before 00bb297).
struct AltSeg {
	char side = 'p';
	int at = 0, len = 0;                 // UTF-16 units into p / s
	std::vector<std::string> opts;       // UTF-8, the forms' differing runs in order
};

// item-mods.ts ModAnchor (p / s in UTF-8; cost in UTF-16 units, the alternation
// group included when `alt` is set)
struct ModAnchor {
	std::string p, s;
	bool caret = false, dollar = false, plus = false;
	int cost = 0;
	std::optional<AltSeg> alt;
};

// item-mods.ts LINE_END (B 189988e): what a fragment's line-end anchor prints --
// the end of the line, or the " (" a fractured / marked line goes on with.
extern const char* const kLineEnd;   // "($| \()"

// item-mods.ts ItemModForms / parseItemModForms (B 00bb297): the arbitration file
// data/regex/item-mod-forms.json for one game, key "<statId>|<ref>" -> the forms
// the site lists (stats.ndjson matcher strings, in order). nullopt = not schema 1
// / not JSON / no such game (the caller then builds without arbitration).
struct ItemModForms {
	std::unordered_map<std::string, std::vector<std::string>> zh, en;
};
std::optional<ItemModForms> ParseItemModForms(const std::string& text, const std::string& game);
// Data\regex_stats\item-mod-forms.json.gz, inflated and parsed (node.ts
// loadItemModData: present -> parsed, missing / broken -> nullopt).
std::optional<ItemModForms> LoadForms(const std::wstring& exeDir, const std::string& game);

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
// `forms` null = no arbitration (B's 4th parameter omitted / null).
Data BuildData(const std::string& game, const std::vector<StatLite>& zh, const std::vector<StatLite>& en,
               const ItemModForms* forms = nullptr);

// LoadStats + LoadForms + BuildData (node.ts loadItemModData). False with *err on a
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
