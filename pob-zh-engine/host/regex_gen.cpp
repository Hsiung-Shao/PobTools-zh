#include "regex_gen.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace RegexGen {
namespace {

// Characters the client's search reads as syntax. A token containing one would
// have to be escaped, and an escape costs more than it saves at these lengths --
// so such tokens are simply not considered. Chinese text almost never contains
// them; English item names never do.
constexpr const char* kMeta = ".^$*+?()[]{}|\\\"!";

// Characters a '#' can turn into once the item is real. Wider than [0-9] on
// purpose: a roll can print a sign, a decimal point or a separator, and the only
// mistake that matters here is calling a token safe when it is not.
constexpr const char* kNumChars = "0123456789.,+-";

inline bool IsNumChar(char c)
{
	return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+';
}

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// The search is case-insensitive, so everything is folded once on the way in and
// the rest of the file can compare bytes.
std::string Fold(const std::string& s)
{
	std::string out = s;
	for (char& c : out) c = LowerAscii(c);
	return out;
}

// Byte offset of each character start, plus the end: characters [a,b) of `s` are
// s.substr(off[a], off[b] - off[a]).
std::vector<size_t> CharOffsets(const std::string& s)
{
	std::vector<size_t> off;
	off.reserve(s.size() + 1);
	for (size_t i = 0; i < s.size();) {
		off.push_back(i);
		unsigned char c = (unsigned char)s[i];
		i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
	}
	off.push_back(s.size());
	return off;
}

// One printed line, cut at the numbers. frags.size() == number of numbers + 1,
// and an empty fragment is meaningful: it says the line starts or ends with one.
struct Line {
	std::vector<std::string> frags;
};

struct Prepped {
	std::vector<Line> lines;
	bool hasNumber = false;   // any line with a '#'; scopes the permissive test
};

Prepped Prep(const std::vector<std::string>& texts)
{
	Prepped p;
	for (const std::string& raw : texts) {
		std::string t = Fold(raw);
		if (t.empty()) continue;
		Line ln;
		size_t start = 0;
		for (size_t i = 0; i <= t.size(); i++) {
			if (i == t.size() || t[i] == '#') {
				ln.frags.push_back(t.substr(start, i - start));
				start = i + 1;
			}
		}
		if (ln.frags.size() > 1) p.hasNumber = true;
		p.lines.push_back(std::move(ln));
	}
	return p;
}

struct Token {
	std::string body;
	bool head = false;
	bool tail = false;

	bool operator==(const Token& o) const
	{
		return head == o.head && tail == o.tail && body == o.body;
	}
};

struct TokenHash {
	size_t operator()(const Token& t) const
	{
		return std::hash<std::string>()(t.body) * 4 + (t.head ? 2u : 0u) + (t.tail ? 1u : 0u);
	}
};

std::string Render(const Token& t)
{
	std::string s;
	if (t.head) s += '^';
	s += t.body;
	if (t.tail) s += '$';
	return s;
}

int TokenChars(const Token& t)
{
	int n = (t.head ? 1 : 0) + (t.tail ? 1 : 0);
	for (char c : t.body) if (((unsigned char)c & 0xC0) != 0x80) n++;
	return n;
}

// ---- matching ---------------------------------------------------------------

// Does this token match every roll of this line? True only when the token sits
// inside one literal run, with any anchor landing on that run's outer edge.
bool AlwaysMatchesLine(const Line& ln, const Token& t)
{
	const size_t n = ln.frags.size();
	if (t.head && t.tail)
		return n == 1 && ln.frags[0] == t.body;
	if (t.head)
		return ln.frags[0].size() >= t.body.size() &&
		       ln.frags[0].compare(0, t.body.size(), t.body) == 0;
	if (t.tail) {
		const std::string& f = ln.frags[n - 1];
		return f.size() >= t.body.size() &&
		       f.compare(f.size() - t.body.size(), t.body.size(), t.body) == 0;
	}
	for (const std::string& f : ln.frags)
		if (f.find(t.body) != std::string::npos) return true;
	return false;
}

// The line flattened to one atom per literal byte plus a wildcard per number.
// Built only for the permissive test, which is the rare path.
struct Atom {
	char ch = 0;
	bool wild = false;
};

std::vector<Atom> Atoms(const Line& ln)
{
	std::vector<Atom> a;
	for (size_t i = 0; i < ln.frags.size(); i++) {
		if (i) a.push_back(Atom{0, true});
		for (char c : ln.frags[i]) a.push_back(Atom{c, false});
	}
	return a;
}

// tok[ti..) against atoms[ai..). `toEnd` demands both run out together, which is
// what a trailing '$' means.
bool MatchFrom(const std::string& tok, size_t ti,
               const std::vector<Atom>& atoms, size_t ai, bool toEnd)
{
	if (ti == tok.size()) return !toEnd || ai == atoms.size();
	if (ai == atoms.size()) return false;
	if (atoms[ai].wild) {
		// A printed number is never empty, so it has to swallow at least one
		// character of the token -- and only characters a number can print. The
		// token may stop part-way through: the rest of the digits are not in it.
		size_t k = 0;
		while (ti + k < tok.size() && IsNumChar(tok[ti + k])) k++;
		for (size_t c = 1; c <= k; c++)
			if (MatchFrom(tok, ti + c, atoms, ai + 1, toEnd)) return true;
		return false;
	}
	if (tok[ti] != atoms[ai].ch) return false;
	return MatchFrom(tok, ti + 1, atoms, ai + 1, toEnd);
}

// Could this token match this line for SOME roll? Used to decide whether a token
// would also hit something the player did not pick, so it errs towards yes.
bool MayMatchLine(const Line& ln, const Token& t)
{
	if (t.body.empty()) return true;
	std::vector<Atom> atoms = Atoms(ln);
	if (t.head) return MatchFrom(t.body, 0, atoms, 0, t.tail);
	for (size_t start = 0; start < atoms.size(); start++) {
		// Starting on a wildcard means the token begins inside the printed
		// number, which MatchFrom already requires it to eat into.
		if (MatchFrom(t.body, 0, atoms, start, t.tail)) return true;
	}
	return false;
}

bool AlwaysMatches(const Prepped& p, const Token& t)
{
	for (const Line& ln : p.lines)
		if (AlwaysMatchesLine(ln, t)) return true;
	return false;
}

bool MayMatch(const Prepped& p, const Token& t)
{
	for (const Line& ln : p.lines)
		if (MayMatchLine(ln, t)) return true;
	return false;
}

// Every token that can be cut out of this entry, handed to `sink`. The same
// enumeration builds the index and proposes candidates, which is what makes an
// index hit mean exactly "AlwaysMatches".
template <class F>
void ForEachToken(const Prepped& p, int maxChars, bool anchors, F&& sink)
{
	for (const Line& ln : p.lines) {
		const size_t nf = ln.frags.size();
		for (size_t fi = 0; fi < nf; fi++) {
			const std::string& f = ln.frags[fi];
			if (f.empty()) continue;
			std::vector<size_t> off = CharOffsets(f);
			const size_t nc = off.size() - 1;
			for (size_t a = 0; a < nc; a++) {
				for (size_t b = a + 1; b <= nc && (int)(b - a) <= maxChars; b++) {
					std::string body = f.substr(off[a], off[b] - off[a]);
					if (body.find_first_of(kMeta) != std::string::npos) continue;
					sink(Token{body, false, false});
					if (!anchors) continue;
					const bool atStart = (fi == 0 && a == 0);
					const bool atEnd = (fi == nf - 1 && b == nc);
					if (atStart) sink(Token{body, true, false});
					if (atEnd) sink(Token{body, false, true});
					if (atStart && atEnd) sink(Token{body, true, true});
				}
			}
		}
	}
}

std::string QuoteIfNeeded(const std::string& term)
{
	return term.find(' ') == std::string::npos ? term : "\"" + term + "\"";
}

std::vector<Token> ParseAlternation(std::string term)
{
	if (!term.empty() && term[0] == '!') term.erase(0, 1);
	std::vector<Token> out;
	size_t start = 0;
	for (size_t i = 0; i <= term.size(); i++) {
		if (i != term.size() && term[i] != '|') continue;
		if (i > start) {
			std::string s = term.substr(start, i - start);
			Token t;
			if (!s.empty() && s[0] == '^') { t.head = true; s.erase(0, 1); }
			if (!s.empty() && s.back() == '$') { t.tail = true; s.pop_back(); }
			t.body = Fold(s);
			out.push_back(std::move(t));
		}
		start = i + 1;
	}
	return out;
}

// Split a query into terms, honouring quotes. Its own tiny parser on purpose:
// Verify must read the string the player will paste.
std::vector<std::string> SplitTerms(const std::string& q)
{
	std::vector<std::string> out;
	std::string cur;
	bool inQuote = false;
	for (char c : q) {
		if (c == '"') { inQuote = !inQuote; continue; }
		if (c == ' ' && !inQuote) {
			if (!cur.empty()) out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += c;
	}
	if (!cur.empty()) out.push_back(cur);
	return out;
}

struct Candidate {
	Token tok;
	const std::vector<int>* hits = nullptr;       // entry indices, ascending
	const std::vector<int>* hiddenHits = nullptr; // entries whose HIDDEN text
	                                              // contains it; null = none
	int cost = 0;                                 // characters, plus the joiner
};

} // namespace

int CharCount(const std::string& utf8)
{
	// Code points, which is what the client counts for everything in the Basic
	// Multilingual Plane -- and nothing the game prints is outside it.
	int n = 0;
	for (char c : utf8) if (((unsigned char)c & 0xC0) != 0x80) n++;
	return n;
}

// ---- Corpus -----------------------------------------------------------------

struct Corpus::Impl {
	std::vector<Entry> entries;
	std::vector<Prepped> prepped;   // printed text: proposes tokens, means "found"
	std::vector<Prepped> hidden;    // per entry, veto only
	Prepped ambient;                // the page's every-item text, veto only
	Options opt;
	// token -> every entry whose printed text literally contains it
	std::unordered_map<Token, std::vector<int>, TokenHash> index;
	// The same for hidden text. Kept apart from `index` on purpose: a hit here
	// must never be credited as covering a pick, because the item may not print
	// that text at all.
	std::unordered_map<Token, std::vector<int>, TokenHash> hiddenIndex;
	std::unordered_set<Token, TokenHash> ambientIndex;
	std::vector<int> numbered;        // entries whose printed text has a '#'
	std::vector<int> numberedHidden;  // entries whose hidden text has a '#'
	// Rare-name seams: every tail of a left word and every head of a right word
	// (folded, up to maxTokenChars characters), plus the whole words for the
	// anchored cases. A token that splits into (tail, head) sits across the
	// space between two random words of some item's name.
	std::unordered_set<std::string> leftTails, rightHeads, leftFull, rightFull;

	// Does the token straddle the seam of a rare name? '^' demands the whole
	// left word (the name starts the line); '$' the whole right word.
	bool JoinsName(const Token& tok) const
	{
		if (leftTails.empty() || rightHeads.empty()) return false;
		const std::vector<size_t> off = CharOffsets(tok.body);
		const size_t nc = off.size() - 1;
		for (size_t k = 1; k < nc; k++) {
			const std::string a = tok.body.substr(0, off[k]);
			const std::string b = tok.body.substr(off[k]);
			if (!(tok.head ? leftFull.count(a) : leftTails.count(a))) continue;
			if (tok.tail ? rightFull.count(b) : rightHeads.count(b)) return true;
		}
		return false;
	}

	// Everything the token hits that the player did not pick. The indexes
	// answer the literal case; the permissive case is only asked of text that
	// prints a number, and only when the token could reach across one.
	//
	// Order matters only for cost: the cheap lookups go first, and a token is
	// rejected by the first thing it hits. Ambient and hidden text are checked
	// before the "cannot cross a number" shortcut so that shortcut stays what
	// it claims to be -- complete knowledge in the literal indexes.
	bool Safe(const Token& tok, const std::vector<int>& hits,
	          const std::vector<bool>& isSelected) const
	{
		for (int h : hits)
			if (!isSelected[h]) return false;
		if (ambientIndex.count(tok)) return false;
		auto hi = hiddenIndex.find(tok);
		if (hi != hiddenIndex.end())
			for (int h : hi->second)
				if (!isSelected[h]) return false;
		if (JoinsName(tok)) return false;
		if (tok.body.find_first_of(kNumChars) == std::string::npos)
			return true;   // cannot cross a number, so the indexes already know
		for (int i : numbered) {
			if (isSelected[i]) continue;
			if (std::binary_search(hits.begin(), hits.end(), i)) continue;
			if (MayMatch(prepped[i], tok)) return false;
		}
		if (ambient.hasNumber && MayMatch(ambient, tok)) return false;
		for (int i : numberedHidden) {
			if (isSelected[i]) continue;
			if (MayMatch(hidden[i], tok)) return false;
		}
		return true;
	}
};

Corpus::Corpus() : impl_(new Impl) {}
Corpus::~Corpus() = default;
Corpus::Corpus(Corpus&&) noexcept = default;
Corpus& Corpus::operator=(Corpus&&) noexcept = default;

bool Corpus::Empty() const { return impl_->entries.empty(); }
size_t Corpus::Size() const { return impl_->entries.size(); }
const Entry& Corpus::At(size_t i) const { return impl_->entries[i]; }

void Corpus::Reset(std::vector<Entry> entries, Ambient ambient, const Options& opt)
{
	Impl& m = *impl_;
	m.entries = std::move(entries);
	m.opt = opt;
	m.opt.maxTokenChars = std::max(1, m.opt.maxTokenChars);
	m.prepped.clear();
	m.hidden.clear();
	m.index.clear();
	m.hiddenIndex.clear();
	m.ambientIndex.clear();
	m.numbered.clear();
	m.numberedHidden.clear();
	m.leftTails.clear();
	m.rightHeads.clear();
	m.leftFull.clear();
	m.rightFull.clear();
	m.prepped.reserve(m.entries.size());
	m.hidden.reserve(m.entries.size());
	for (const Entry& e : m.entries) {
		m.prepped.push_back(Prep(e.texts));
		m.hidden.push_back(Prep(e.hidden));
	}
	for (int i = 0; i < (int)m.prepped.size(); i++) {
		if (m.prepped[i].hasNumber) m.numbered.push_back(i);
		std::unordered_set<Token, TokenHash> seen;
		ForEachToken(m.prepped[i], m.opt.maxTokenChars, m.opt.anchors,
		             [&](const Token& t) {
			if (seen.insert(t).second) m.index[t].push_back(i);
		});
		// Same enumeration over the hidden lines, so a literal lookup is as
		// complete for them as it is for the printed ones -- but into a map of
		// its own (see Impl).
		if (m.hidden[i].hasNumber) m.numberedHidden.push_back(i);
		std::unordered_set<Token, TokenHash> seenHidden;
		ForEachToken(m.hidden[i], m.opt.maxTokenChars, m.opt.anchors,
		             [&](const Token& t) {
			if (seenHidden.insert(t).second) m.hiddenIndex[t].push_back(i);
		});
	}
	m.ambient = Prep(ambient.lines);
	ForEachToken(m.ambient, m.opt.maxTokenChars, m.opt.anchors,
	             [&](const Token& t) { m.ambientIndex.insert(t); });
	// Name seams. Every piece that could be one side of a straddling token: up
	// to maxTokenChars characters, the whole word included.
	auto sides = [&](const std::vector<std::string>& words, bool left) {
		for (const std::string& raw : words) {
			const std::string s = Fold(raw);
			if (s.empty()) continue;
			(left ? m.leftFull : m.rightFull).insert(s);
			const std::vector<size_t> off = CharOffsets(s);
			const size_t nc = off.size() - 1;
			for (size_t k = 1; k <= nc && (int)k <= m.opt.maxTokenChars; k++) {
				if (left) m.leftTails.insert(s.substr(off[nc - k]));
				else      m.rightHeads.insert(s.substr(0, off[k]));
			}
		}
	};
	sides(ambient.nameLeft, true);
	sides(ambient.nameRight, false);
}

Result Corpus::Build(const std::vector<int>& selected, Mode mode) const
{
	const Impl& m = *impl_;
	Result r;
	if (selected.empty() || m.entries.empty()) return r;

	std::vector<bool> isSelected(m.entries.size(), false);
	std::vector<int> picks;
	for (int i : selected) {
		if (i < 0 || i >= (int)m.entries.size() || isSelected[i]) continue;
		isSelected[i] = true;
		picks.push_back(i);
	}
	std::sort(picks.begin(), picks.end());
	if (picks.empty()) return r;

	// Candidates come only from what the player picked; nothing else could match
	// them in the first place.
	std::unordered_set<Token, TokenHash> proposed;
	std::vector<Candidate> cands;
	for (int s : picks) {
		ForEachToken(m.prepped[s], m.opt.maxTokenChars, m.opt.anchors,
		             [&](const Token& t) {
			if (!proposed.insert(t).second) return;
			auto it = m.index.find(t);
			if (it == m.index.end()) return;   // cannot happen; cheap to survive
			if (!m.Safe(t, it->second, isSelected)) return;
			auto hh = m.hiddenIndex.find(t);
			cands.push_back(Candidate{t, &it->second,
			                          hh == m.hiddenIndex.end() ? nullptr : &hh->second,
			                          TokenChars(t) + 1});
		});
	}

	if (mode == Mode::All) {
		// AND means every pick needs a term of its own, and that term must not be
		// satisfiable by any OTHER entry -- a token shared by two picks would let
		// either one alone satisfy the pair. Hidden text counts here too: Safe
		// only ruled out UNPICKED entries, and an item carrying pick B alone
		// would satisfy A's term through B's reminder text.
		for (int s : picks) {
			const Candidate* best = nullptr;
			for (const Candidate& c : cands) {
				if (c.hits->size() != 1 || (*c.hits)[0] != s) continue;
				bool ownHiddenOnly = true;
				if (c.hiddenHits)
					for (int h : *c.hiddenHits)
						if (h != s) { ownHiddenOnly = false; break; }
				if (!ownHiddenOnly) continue;
				if (!best || c.cost < best->cost ||
				    (c.cost == best->cost && Render(c.tok) < Render(best->tok)))
					best = &c;
			}
			if (best) r.tokens.push_back(Render(best->tok));
			else r.unresolved.push_back(s);
		}
		for (const std::string& t : r.tokens) {
			if (!r.query.empty()) r.query += ' ';
			r.query += QuoteIfNeeded(t);
		}
	} else {
		// OR: take the token that covers the most picks per character until none
		// covers anything new. Greedy weighted set cover -- the exact answer is
		// NP-hard and greedy is within a whisker at these sizes.
		std::vector<bool> done(m.entries.size(), false);
		size_t left = picks.size();
		while (left > 0) {
			const Candidate* best = nullptr;
			double bestScore = 0.0;
			for (const Candidate& c : cands) {
				int fresh = 0;
				for (int h : *c.hits) if (isSelected[h] && !done[h]) fresh++;
				if (fresh == 0) continue;
				const double score = (double)fresh / (double)c.cost;
				// Ties broken by cost then by text, so the same picks always
				// produce the same string. A query that drifted between runs
				// would be impossible to tell apart from a data change.
				if (!best || score > bestScore ||
				    (score == bestScore && (c.cost < best->cost ||
				     (c.cost == best->cost && Render(c.tok) < Render(best->tok))))) {
					best = &c;
					bestScore = score;
				}
			}
			if (!best) break;
			r.tokens.push_back(Render(best->tok));
			for (int h : *best->hits)
				if (isSelected[h] && !done[h]) { done[h] = true; left--; }
		}
		for (int s : picks) if (!done[s]) r.unresolved.push_back(s);

		std::string alt;
		for (const std::string& t : r.tokens) {
			if (!alt.empty()) alt += '|';
			alt += t;
		}
		if (!alt.empty()) {
			// Always quoted, negation included: the client reads an unquoted
			// space as a term break, and two characters is a cheap price against
			// a failure that is invisible until the search returns the wrong tab.
			r.query = "\"" + std::string(mode == Mode::None ? "!" : "") + alt + "\"";
		}
	}

	r.length = CharCount(r.query);
	r.exact = r.unresolved.empty();
	return r;
}

Check Corpus::Verify(const std::vector<int>& selected, const std::string& query) const
{
	const Impl& m = *impl_;
	Check chk;
	std::vector<bool> want(m.entries.size(), false);
	for (int i : selected)
		if (i >= 0 && i < (int)m.entries.size()) want[i] = true;

	const std::vector<std::string> terms = SplitTerms(query);
	if (terms.empty()) {
		for (int i = 0; i < (int)m.entries.size(); i++)
			if (want[i]) chk.missing.push_back(i);
		chk.ok = chk.missing.empty();
		return chk;
	}

	// definite[i] = some term certainly finds entry i; possible[i] = some term
	// might. The two differ only around numbers, and that gap is the point: a
	// miss is judged by what is certain, an over-match by what is possible.
	std::vector<bool> definite(m.entries.size(), false), possible(m.entries.size(), false);
	for (const std::string& term : terms) {
		const std::vector<Token> alts = ParseAlternation(term);
		for (int e = 0; e < (int)m.entries.size(); e++) {
			for (const Token& t : alts) {
				// "Certainly finds" reads printed text only; "might hit" reads
				// the hidden text as well. The asymmetry is the same one as
				// around numbers, and for the same reason.
				if (AlwaysMatches(m.prepped[e], t)) definite[e] = true;
				if (MayMatch(m.prepped[e], t) || MayMatch(m.hidden[e], t)) possible[e] = true;
			}
		}
		for (const Token& t : alts)
			if (MayMatch(m.ambient, t) || m.JoinsName(t)) chk.ambient.push_back(Render(t));
	}

	for (int e = 0; e < (int)m.entries.size(); e++) {
		if (want[e] && !definite[e]) chk.missing.push_back(e);
		if (!want[e] && possible[e]) chk.extra.push_back(e);
	}
	chk.ok = chk.missing.empty() && chk.extra.empty() && chk.ambient.empty();
	return chk;
}

} // namespace RegexGen
