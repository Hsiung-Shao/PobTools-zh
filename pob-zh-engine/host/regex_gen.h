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
	// Picks that no token can single out, by index into the corpus. Always
	// because another entry prints the same line: nothing can tell them apart,
	// and saying so is better than shipping a query that quietly over-matches.
	std::vector<int> unresolved;
	bool exact = true;                  // false <=> unresolved is non-empty
};

// What a query actually selects, recomputed from the query text. Verify reads
// the string the player will paste rather than the structure Build had in mind,
// so a bug in the generator cannot hide behind agreeing with itself.
struct Check {
	bool ok = false;
	std::vector<int> missing;   // picked, but the query does not find it
	std::vector<int> extra;     // not picked, but the query finds it
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

	void Reset(std::vector<Entry> entries, const Options& opt = Options{});
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
