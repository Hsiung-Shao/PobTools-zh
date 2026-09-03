// Search-string generator for Path of Exile's in-game search boxes.
//
// The player's problem: the stash search field takes 250 characters and they
// want it to find exactly nine map modifiers out of a hundred and twenty. Typing
// the modifiers out does not fit. What fits is, for each one, the shortest piece
// of its text that no OTHER modifier contains -- three characters instead of
// twenty, and never a false positive.
//
// Finding those pieces is what this file does. It is deliberately free of ImGui,
// of files and of Windows, so --regex-selftest can drive it directly.
//
// ---- the dialect --------------------------------------------------------------
//
// The client's search is not a full regex engine, and the generator only ever
// emits the part of it that is safe:
//
//     term term term      every term must match           (AND)
//     "a b"               one term, spaces included
//     a|b|c               one term, any alternative       (OR)
//     !a                  the term must NOT match
//
// So "any of these" is one alternation, "none of these" is that alternation
// negated, and "all of these" is one term each. Nothing else is generated -- no
// character classes, no quantifiers, no groups -- because a token that needed
// them would be longer than the literal it replaced.
//
// ---- why numbers are wildcards ------------------------------------------------
//
// Entry text stores '#' where the game prints a rolled number. A token must
// never straddle one: "increased 3" matches a map that rolled 30% and misses
// every other map. Two consequences, and the asymmetry between them is the whole
// safety argument:
//
//   * a token is only ever CUT from inside one literal run, so it matches every
//     roll of the entry it came from -- the strict test;
//   * but when asking whether a token would ALSO hit something the player did
//     not pick, '#' has to stand for any number, because "2 seconds" really does
//     occur in "# seconds" once the # rolls a 2 -- the permissive test.
//
// Rejecting on the permissive test and accepting on the strict one is the safe
// direction: the generator can only ever refuse a token it could have used, and
// never emit one that quietly over-matches.
//
// ---- what the search really reads --------------------------------------------
//
// The client matches more than the modifier lines, and it does so whether or
// not that text is on screen (verified in-game, 2026-09: a map whose only line
// is 怪物傷害必定造成點燃 is found by "常" through its tag 異常狀態):
//
//   * the advanced description of each modifier -- 前綴 "燃燒的"(階層：1) and
//     the tag names after it (元素,火焰,異常狀態);
//   * the reminder text attached to the line (時空鎖鏈's explanation contains
//     平常 and 失效);
//   * the item's name: affix name + base for a magic item, two random words
//     for a rare one;
//   * the lines every item of the kind carries: 怪物等級, 物品數量, 已汙染, the
//     base name, the flavour paragraph.
//
// So an entry carries `hidden` text next to its printed `texts`, and a corpus
// carries the page's `Ambient` text. The rule that keeps the two tests honest:
// printed text PROPOSES tokens and is what "found it" means; hidden and ambient
// text can only VETO a token, never source one and never count as a hit.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace RegexGen {

// One searchable thing: a map modifier, an item base, a line the player pasted.
// `texts` are the lines the game prints for it, with '#' where a number goes.
// Matching the entry means matching ANY of them -- a two-stat modifier prints
// two lines and the player sees either.
struct Entry {
	std::string id;
	std::vector<std::string> texts;
	// Text the search also reads for this entry but the item does not print as
	// a modifier line: the advanced description, its tags, the reminder text,
	// the name pieces. Never cut into tokens and never "finds" the entry; only
	// consulted for "would this token also hit something else". '#' works here
	// exactly as it does in `texts`.
	std::vector<std::string> hidden;
};

// Text present on EVERY item of the page, so any token that hits it hits
// everything. `lines` are matched like hidden lines. `nameLeft` / `nameRight`
// are the random words a rare item's name is assembled from (the right half
// keeps its leading space): the join of any left word and any right word is
// also on the item, and a token can straddle that seam, so the seam is checked
// as well as the words themselves.
struct Ambient {
	std::vector<std::string> lines;
	std::vector<std::string> nameLeft;
	std::vector<std::string> nameRight;
};

enum class Mode {
	Any,    // "a|b"    -- items carrying at least one of the picks
	All,    // a b      -- items carrying every pick
	None,   // "!a|b"   -- items carrying none of them
};

struct Options {
	// Longest token to consider, in characters (not bytes). Beyond about a dozen
	// a token is no longer an abbreviation, and every extra character multiplies
	// the index for nothing.
	int  maxTokenChars = 12;
	// '^' and '$' anchor to the ends of one printed line, which is often what
	// makes a token unique at all ("r damage$" beats spelling out the phrase).
	bool anchors = true;
};

struct Result {
	std::string query;                  // paste this into the game
	int         length = 0;             // characters, counted as the client does
	std::vector<std::string> tokens;    // the pieces, for showing the work
	// Picks that no token can single out, by index into the corpus. Either
	// another entry prints the same line, or every usable fragment of this one
	// also occurs in text the page's items all carry (or in another entry's
	// hidden text): nothing can tell them apart, and saying so is better than
	// shipping a query that quietly over-matches.
	std::vector<int> unresolved;
	bool exact = true;                  // false <=> unresolved is non-empty
};

// What a query actually selects, recomputed from the query text. Verify reads
// the string the player will paste rather than the structure Build had in mind,
// so a bug in the generator cannot hide behind agreeing with itself.
struct Check {
	bool ok = false;
	std::vector<int> missing;   // picked, but the query does not find it
	std::vector<int> extra;     // not picked, but the query finds it (printed
	                            // or hidden text)
	// Alternatives that hit the page's ambient text, i.e. every item. Named
	// separately because the honest `extra` for such a term is "the whole
	// list", which tells the reader nothing about which term did it.
	std::vector<std::string> ambient;
};

// A prepared corpus. Preparing costs a pass over every entry and builds the
// substring index that makes "does this token hit anything else?" a lookup
// instead of a scan; with five thousand item bases that is the difference
// between an instant answer and a stalled frame. So it is done once, on load,
// and every Build() against it is cheap.
class Corpus {
public:
	Corpus();
	~Corpus();
	Corpus(Corpus&&) noexcept;
	Corpus& operator=(Corpus&&) noexcept;

	void Reset(std::vector<Entry> entries, Ambient ambient = Ambient{},
	           const Options& opt = Options{});
	bool Empty() const;
	size_t Size() const;
	const Entry& At(size_t i) const;

	Result Build(const std::vector<int>& selected, Mode mode) const;
	Check  Verify(const std::vector<int>& selected, const std::string& query) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Characters as the client counts them, i.e. code points rather than bytes --
// a Chinese token costs one per character, which is most of why the Chinese
// query fits where the English one does not.
int CharCount(const std::string& utf8);

} // namespace RegexGen
