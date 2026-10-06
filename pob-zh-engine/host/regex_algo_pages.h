// Algorithmic pages of the Poe Regex tool: the numeric section on top of the
// map / waystone modifier pages, and the vendor page. Ported from exile-appraiser
// `regex/src/pages/{types,index,numeric-pages,vendor-pages}.ts`, `sections.ts`,
// `view.ts` (condText / sectionSummary) and `combine.ts` (multi-page merge, R4);
// every function names the TS file:line it mirrors, and regex_r3_golden.inc (made
// by running that TS over our own Data files) holds them to identical output.
//
// An algorithmic page looks like a corpus page from the outside -- a list of
// entries with a zh / en line -- but its terms are not cut by the cover algorithm
// in regex_gen. Each ticked entry has an input (a range, a choice, colours) and
// a fragment builder (regex_frag), and the fragment is one search term of its own.
//
// Two pages per game:
//   map_numeric / waystone_numeric   the numeric SECTION of map_mods / waystone_mods
//                                    (`sectionOf`); never in the page list, its
//                                    output joins the host page's in one string
//   vendor_items / vendor_items_poe2 a page of its own in the list
//
// Pure: no ImGui, no files. The panel (regex_tool_ui.cpp) and --regex-selftest
// both drive this.
#pragma once

#include "regex_data.h"
#include "regex_frag.h"
#include "regex_gen.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RegexAlgo {

using RegexFrag::AlgoValue;
using RegexFrag::Lang;
using RegexFrag::RangeOp;

// pages/types.ts:29 AlgoInput kinds.
enum class InputKind { Range, Select, Colors, Count };

// pages/types.ts:23 AlgoOption
struct AlgoOption {
	std::string id, zh, en;
};

// pages/types.ts:29 AlgoInput, flattened: the fields a kind does not use stay at
// their defaults.
struct AlgoInput {
	InputKind kind = InputKind::Range;
	int digits = 3;                     // Range
	bool percent = false;               // Range
	std::vector<RangeOp> ops;           // Range: the operators offered, in order
	int lo = 0, hi = 0;                 // Range / Count
	std::vector<AlgoOption> options;    // Select / Count
	int maxTotal = 0;                   // Colors
	AlgoValue def;
};

using FragmentFn = std::function<std::optional<std::string>(const AlgoValue&, Lang)>;
using OwnLineFn = std::function<bool(const std::string&)>;

// pages/types.ts:37 AlgoEntry. `def` carries id / group / zh[0] / en[0] like a
// corpus entry, so row text and keys work the same way.
struct AlgoEntry {
	RegexEntryDef def;
	AlgoInput input;
	bool untested = false;   // the client display is an assumption not yet seen in game
	FragmentFn fragment;     // nullopt = the input does not describe anything
	// The corpus line (with '#') is itself what this entry matches (the tier
	// fragment and map names): skipped by the fragment conflict check.
	OwnLineFn ownLine;
};

// pages/types.ts:50 AlgoPage
struct AlgoPage {
	std::string id, game, title, titleEn, note;
	RegexPageKind kind = RegexPageKind::Numeric;
	std::string sectionOf;   // host page id for an embedded numeric section; empty otherwise
	int limit = 250;
	std::vector<std::string> groups, groupsEn;
	std::vector<AlgoEntry> entries;
};

// numeric-pages.ts:58 / vendor-pages.ts:20: the clientstrings keys each game needs.
std::vector<std::string> NumericLabelKeys(const std::string& game);
std::vector<std::string> VendorLabelKeys(const std::string& game);

// numeric-pages.ts:123 numericPages / vendor-pages.ts:36 vendorPages /
// pages/index.ts:22 algoPages (numeric first, then vendor). `labels` null or
// not present (schema 1) = no pages; a missing key drops that entry (never a
// guessed translation).
std::vector<AlgoPage> NumericPages(const std::string& game, const RegexLabels* labels);
std::vector<AlgoPage> VendorPages(const std::string& game, const RegexLabels* labels);
std::vector<AlgoPage> AlgoPages(const std::string& game, const RegexLabels* labels);

extern const char* const kRarityEntryId;   // numeric-pages.ts:92 "item_rarity_class"

// ---- sections.ts ------------------------------------------------------------

// sections.ts:16 SECTION_HOSTS: section page id -> host page id; "" if not a section.
std::string SectionHostOf(const std::string& pageId);
// sections.ts:26 sectionIdOf: host page id -> its section's id; "" if none.
std::string SectionIdOf(const std::string& hostId);
// sections.ts:33 numericKeyOf: values of a section live under the host page id.
std::string NumericKeyOf(const std::string& pageId);

// ---- page list (pages/index.ts) ---------------------------------------------

// One page of a game's catalogue, corpus or algorithmic. Exactly one pointer is set.
struct PageRef {
	const RegexPageDef* corpus = nullptr;
	const AlgoPage* algo = nullptr;
	const std::string& Id() const { return corpus ? corpus->id : algo->id; }
	const std::string& Game() const { return corpus ? corpus->game : algo->game; }
	const std::string& Title() const { return corpus ? corpus->title : algo->title; }
	int Limit() const { return corpus ? corpus->limit : algo->limit; }
	size_t Size() const { return corpus ? corpus->entries.size() : algo->entries.size(); }
	bool IsSection() const { return algo && !algo->sectionOf.empty(); }
};

// pages/index.ts:27 listedPages: the page list without embedded sections.
std::vector<PageRef> ListedPages(const std::vector<PageRef>& pages);
// pages/index.ts:37 sectionPageOf: the host's section in the same game; nullptr if none.
const AlgoPage* SectionPageOf(const std::vector<PageRef>& pages, const std::string& hostId,
                              const std::string& game = std::string());
// pages/index.ts:45 hostIdOf: a section's id -> its host; anything else -> itself.
std::string HostIdOf(const std::vector<PageRef>& pages, const std::string& id);
// pages/index.ts:55 combineOrder: listed pages in order, each host followed by its
// section; `only` = just that page (+ its section), i.e. the single-page output.
std::vector<PageRef> CombineOrder(const std::vector<PageRef>& pages, const std::string* only = nullptr);

// ---- values and the row editor (RegexAlgoList.vue, store.ts) ------------------

using ValueMap = std::map<std::string, AlgoValue>;   // entry id -> value

// store.ts:375 valueOf: the stored value, or the entry's default.
const AlgoValue& ValueOf(const ValueMap& m, const AlgoEntry& e);
// pages/index.ts:105 valueUsable
bool ValueUsable(const AlgoEntry& e, const AlgoValue& v, Lang lang = Lang::Zh);

// RegexAlgoList.vue opOf: the operator a range row shows -- the value's own when
// the entry offers it, else the first one offered.
RangeOp OpOf(const AlgoEntry& e, const AlgoValue& v);
// RegexAlgoList.vue setOp: switch operator keeping the numbers (range = sorted pair).
AlgoValue WithOp(const AlgoEntry& e, const AlgoValue& cur, RangeOp op);
// RegexAlgoList.vue setNum: `n` nullopt = the field was cleared; truncated; a
// range row keeps its operator (>= drops max, <= drops min).
AlgoValue WithNum(const AlgoEntry& e, const AlgoValue& cur, bool isMax, std::optional<double> n);
// RegexAlgoList.vue setChoice
AlgoValue WithChoice(const AlgoValue& cur, const std::string& id);
// RegexAlgoList.vue colorCount / setColor: link colours as a letter count (0..6 each).
int ColorCount(const AlgoValue& v, char c);
AlgoValue WithColor(const AlgoValue& cur, char c, int n);

// Ticks + values of one algorithmic page. store.ts:485 setValue: editing a value
// ticks the row ("改值自動勾選").
struct AlgoSelection {
	std::vector<char> picked;   // parallel to the page's entries
	ValueMap values;
	void Reset(size_t n) { picked.assign(n, 0); values.clear(); }
	// Returns true when this edit also ticked the row.
	bool SetValue(const AlgoPage& page, int idx, const AlgoValue& v, bool tick = true);
	int Count() const;
	std::vector<int> Picks() const;
};

// ---- view.ts ------------------------------------------------------------------

// view.ts:111 condText: ">=16", "<=5%", "10–20%", an option's text; nullopt = invalid.
std::optional<std::string> CondText(const AlgoEntry& e, const AlgoValue& v, Lang lang);

// view.ts:122 SectionSummaryItem / :131 sectionSummary: ticked entries in row order.
struct SummaryItem {
	std::string id, label;
	std::optional<std::string> cond;
};
std::vector<SummaryItem> SectionSummary(const AlgoPage& page, const std::vector<int>& picked,
                                        const ValueMap& values, Lang labelLang);

// ---- combine (combine.ts): several pages, custom text, excludes -----------------

// regex_tool_ui.cpp buildCorpus / data.ts:243 buildCorpus: the whole page in one language.
void BuildPageCorpus(const RegexPageDef& page, Lang lang, RegexGen::Corpus& out);

// combine.ts:18 CombineSel. `corpus` is the page's corpus in the output language
// (corpus pages only; the caller caches it). Null = Combine builds one for this call.
struct CombineSel {
	PageRef page;
	std::vector<int> picks;
	const ValueMap* values = nullptr;
	const RegexGen::Corpus* corpus = nullptr;
};

// combine.ts:33 PageContribution
struct PageContribution {
	std::string id;
	RegexPageKind kind = RegexPageKind::Mods;
	int picked = 0;
	int length = 0;       // fragments + one separator each
	int unresolved = 0;   // corpus: picks no token singles out; algo: invalid inputs
	std::vector<std::string> fragments;
};

// combine.ts:46 ConflictKind
enum class ConflictKind { Extra, Missing, Ambient, Fragment, Exclude, Invalid };
const char* ConflictKindId(ConflictKind k);   // "extra" ... as in combine.ts:46

// combine.ts:48 Conflict. `page` / `entry` empty when the TS leaves them undefined.
struct Conflict {
	ConflictKind kind;
	std::string page, entry, text;
};

// combine.ts:63-64 custom / excludes items (custom terms are always unverified).
struct CustomTerm {
	std::string text, term;
};
struct ExcludeToken {
	std::string text, token;
};

// combine.ts:58 CombineResult
struct CombineResult {
	std::string query;
	int length = 0;
	int limit = 250;
	std::vector<PageContribution> perPage;
	std::vector<CustomTerm> custom;
	std::vector<ExcludeToken> excludes;
	int customLength = 0;     // each term (quoted if needed) + 1
	int excludesLength = 0;   // each token + 1
	std::string verifyQuery;
	RegexGen::Check check;
	std::vector<Conflict> conflicts;
	bool ok = true;
	// The FIRST corpus page's Build() result (unresolved picks, tokens) for the
	// panel's single-page details; empty when no corpus page had picks.
	RegexGen::Result corpusResult;
	bool hasCorpus = false;
};

// JS String.prototype.trim: WhiteSpace + LineTerminator code points off both ends
// (store.ts addCustom trims the typed text with it; escapeTerm trims again).
std::string JsTrim(const std::string& s);

// combine.ts:85 escapeTerm: user text -> a literal search term. Drops '"' (term
// boundary), trims (JS String.prototype.trim whitespace), escapes regex syntax
// \ ^ $ . | ? * + ( ) [ ] { }, and escapes a leading '!'.
std::string EscapeTerm(const std::string& s);

// combine.ts:98-135 unionCorpus and its cache: the corpus pages taking part,
// entries and ambient text concatenated, entry ids "<page>:<entry>". Kept by the
// caller (the panel) so repeated recomputes with the same pages do not rebuild
// the index; the TS keeps the last 4 (combine.ts:133), and so does this.
class UnionCorpusCache {
public:
	struct Union {
		std::vector<const RegexPageDef*> pages;
		Lang lang = Lang::Zh;
		RegexGen::Corpus corpus;
		std::vector<int> offsets;
		std::vector<std::pair<std::string, std::string>> owner;   // union index -> (page id, entry id)
	};
	// Two or more pages; a single page goes through the page's own corpus.
	const Union& Get(const std::vector<const RegexPageDef*>& pages, Lang lang);
	void Clear() { items_.clear(); }

private:
	std::vector<std::unique_ptr<Union>> items_;   // most recent first
};

// combine.ts:148 combine. Corpus pages follow `mode` (any: all tokens in ONE
// "a|b" term; all: one term per token; none: all into the single "!a|b" term),
// each algorithmic pick is a term of its own, custom text is escaped into
// terms of its own (unverified), excludes join the "!" term. Order: any, all,
// algorithmic, custom, none. Verify runs over the union of the corpus pages;
// fragments and excludes are checked line by line with the R1 matcher.
// `cache` null = the union is built for this call only.
CombineResult Combine(Lang lang, RegexGen::Mode mode, const std::vector<CombineSel>& sels,
                      const std::vector<std::string>& custom = {},
                      const std::vector<std::string>& excludes = {},
                      UnionCorpusCache* cache = nullptr);

// The single-page output (store.ts pageCombined): Combine with no custom text
// and no excludes. Kept as a name because the panel and R3 tests use it.
inline CombineResult CombineSingle(Lang lang, RegexGen::Mode mode, const std::vector<CombineSel>& sels)
{
	return Combine(lang, mode, sels);
}

} // namespace RegexAlgo
