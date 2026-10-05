#include "regex_match.h"

#include <algorithm>

// See regex_match.h for the supported syntax and the reasons this exists.
// The structure follows the ECMAScript spec's matcher/continuation model
// (22.2.2 "Pattern Semantics"): every node is matched with "what comes after"
// passed down as a continuation, so backtracking is plain recursion and the
// RepeatMatcher's empty-iteration rule can be copied word for word.

namespace {

using Range = Rx::Range;
using Node = Rx::Node;

constexpr char32_t kMaxCp = 0x10FFFF;

bool IsAsciiLetter(char32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }
bool IsWord(char32_t c) { return IsDigit(c) || IsAsciiLetter(c) || c == '_'; }
bool IsLineTerm(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

// ES Canonicalize (22.2.2.7.3), non-unicode mode, for the BMP: toUpperCase of
// the single code unit; a result longer than one unit, or a non-ASCII unit that
// would become ASCII, leaves the character alone. The table is generated from
// node (regex_case_table.inc) rather than taken from Windows: LCMapStringEx
// differs from it in 247 units (older Unicode, no titlecase digraphs, U+00B5),
// and Wine's table may differ again.
#include "regex_case_table.inc"

struct CaseTable {
	std::vector<char16_t> up;                               // 0..0xFFFF
	std::vector<std::pair<char16_t, char16_t>> inv;         // (canon, ch), canon != ch, sorted
};

const CaseTable& Cases()
{
	static const CaseTable table = [] {
		CaseTable t;
		t.up.resize(0x10000);
		for (size_t i = 0; i < t.up.size(); i++) t.up[i] = (char16_t)i;
		for (const auto& p : kRxCanonPairs) {
			t.up[p[0]] = (char16_t)p[1];
			t.inv.emplace_back((char16_t)p[1], (char16_t)p[0]);
		}
		std::sort(t.inv.begin(), t.inv.end());
		return t;
	}();
	return table;
}

char32_t Canon(char32_t c) { return c > 0xFFFF ? c : (char32_t)Cases().up[c]; }

// ECMAScript WhiteSpace + LineTerminator (what \s matches).
const std::vector<Range>& SpaceRanges()
{
	static const std::vector<Range> r = {
		{0x09, 0x0D}, {0x20, 0x20}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A},
		{0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF},
	};
	return r;
}
const std::vector<Range>& DigitRanges() { static const std::vector<Range> r = {{'0', '9'}}; return r; }
const std::vector<Range>& WordRanges()
{
	static const std::vector<Range> r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
	return r;
}

std::vector<Range> Normalize(std::vector<Range> v)
{
	std::sort(v.begin(), v.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
	std::vector<Range> out;
	for (const Range& r : v) {
		if (!out.empty() && r.lo <= out.back().hi + 1) out.back().hi = std::max(out.back().hi, r.hi);
		else out.push_back(r);
	}
	return out;
}

std::vector<Range> Complement(const std::vector<Range>& in)
{
	std::vector<Range> v = Normalize(in), out;
	char32_t next = 0;
	for (const Range& r : v) {
		if (r.lo > next) out.push_back({next, r.lo - 1});
		next = r.hi + 1;
	}
	if (next <= kMaxCp) out.push_back({next, kMaxCp});
	return out;
}

bool InRanges(const std::vector<Range>& v, char32_t c)
{
	// Ranges are normalized (sorted, disjoint): binary search.
	size_t lo = 0, hi = v.size();
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (c < v[mid].lo) hi = mid;
		else if (c > v[mid].hi) lo = mid + 1;
		else return true;
	}
	return false;
}

int HexVal(char32_t c)
{
	if (c >= '0' && c <= '9') return (int)(c - '0');
	if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
	return -1;
}

// ---- parser -----------------------------------------------------------------

class Parser {
public:
	Parser(const std::u32string& p, Rx& rx) : p_(p), rx_(rx) {}

	bool Run(std::string* err)
	{
		int root = ParseAlt();
		if (!err_.empty()) { if (err) *err = err_; return false; }
		if (i_ < p_.size()) {   // only a ')' stops ParseAlt early
			Fail("Unmatched ')'");
			if (err) *err = err_;
			return false;
		}
		rx_.root = root;
		return true;
	}

private:
	const std::u32string& p_;
	Rx& rx_;
	size_t i_ = 0;
	std::string err_;

	bool AtEnd() const { return i_ >= p_.size(); }
	char32_t Peek(size_t o = 0) const { return i_ + o < p_.size() ? p_[i_ + o] : 0; }
	void Fail(const std::string& m)
	{
		if (err_.empty()) err_ = m + " at position " + std::to_string(i_);
	}

	int Add(Node n)
	{
		rx_.nodes.push_back(std::move(n));
		return (int)rx_.nodes.size() - 1;
	}
	int AddClass(std::vector<Range> ranges, bool negated)
	{
		rx_.classes.push_back(Normalize(std::move(ranges)));
		Node n;
		n.kind = Node::Class;
		n.cls = (int)rx_.classes.size() - 1;
		n.negated = negated;
		return Add(std::move(n));
	}
	int AddChar(char32_t c)
	{
		Node n;
		n.kind = Node::Char;
		n.ch = rx_.flags.icase ? Canon(c) : c;
		return Add(std::move(n));
	}
	int AddKind(Node::Kind k)
	{
		Node n;
		n.kind = k;
		return Add(std::move(n));
	}

	int ParseAlt()
	{
		std::vector<int> alts;
		alts.push_back(ParseSeq());
		while (err_.empty() && !AtEnd() && Peek() == '|') {
			i_++;
			alts.push_back(ParseSeq());
		}
		if (alts.size() == 1) return alts[0];
		Node n;
		n.kind = Node::Alt;
		n.kids = std::move(alts);
		return Add(std::move(n));
	}

	int ParseSeq()
	{
		std::vector<int> items;
		while (err_.empty() && !AtEnd() && Peek() != '|' && Peek() != ')') {
			int t = ParseTerm();
			if (t < 0) break;
			items.push_back(t);
		}
		if (items.size() == 1) return items[0];
		Node n;
		n.kind = items.empty() ? Node::Empty : Node::Seq;
		n.kids = std::move(items);
		return Add(std::move(n));
	}

	// Is there a {n} / {n,} / {n,m} at i_? Fills min/max and the length.
	bool QuantBraces(int& mn, int& mx, size_t& len) const
	{
		size_t j = i_;
		if (j >= p_.size() || p_[j] != '{') return false;
		j++;
		auto num = [&](long long& v) {
			size_t s = j;
			v = 0;
			while (j < p_.size() && IsDigit(p_[j])) {
				v = std::min<long long>(v * 10 + (p_[j] - '0'), 0x7FFFFFFF);
				j++;
			}
			return j > s;
		};
		long long a = 0, b = -1;
		if (!num(a)) return false;
		if (j < p_.size() && p_[j] == ',') {
			j++;
			long long t = 0;
			if (num(t)) b = t;
			else b = -1;
		} else {
			b = a;
		}
		if (j >= p_.size() || p_[j] != '}') return false;
		j++;
		mn = (int)a;
		mx = (int)b;
		len = j - i_;
		return true;
	}

	bool AtQuantifier() const
	{
		if (AtEnd()) return false;
		char32_t c = Peek();
		if (c == '*' || c == '+' || c == '?') return true;
		int a, b;
		size_t len;
		return c == '{' && QuantBraces(a, b, len);
	}

	int ParseTerm()
	{
		bool assertion = false;
		int atom = ParseAtom(assertion);
		if (atom < 0 || !err_.empty()) return -1;
		if (!AtQuantifier()) return atom;
		if (assertion) {
			Fail("Nothing to repeat");
			return -1;
		}
		int mn = 0, mx = -1;
		char32_t c = Peek();
		if (c == '*') { mn = 0; mx = -1; i_++; }
		else if (c == '+') { mn = 1; mx = -1; i_++; }
		else if (c == '?') { mn = 0; mx = 1; i_++; }
		else {
			size_t len = 0;
			QuantBraces(mn, mx, len);
			if (mx >= 0 && mx < mn) {
				Fail("numbers out of order in {} quantifier");
				return -1;
			}
			i_ += len;
		}
		bool greedy = true;
		if (!AtEnd() && Peek() == '?') { greedy = false; i_++; }
		if (AtQuantifier()) {
			Fail("Nothing to repeat");
			return -1;
		}
		Node n;
		n.kind = Node::Repeat;
		n.min = mn;
		n.max = mx;
		n.greedy = greedy;
		n.kids.push_back(atom);
		return Add(std::move(n));
	}

	int ParseAtom(bool& assertion)
	{
		char32_t c = Peek();
		switch (c) {
		case '^': i_++; assertion = true; return AddKind(Node::Begin);
		case '$': i_++; assertion = true; return AddKind(Node::End);
		case '.': i_++; return AddKind(Node::Any);
		case '*': case '+': case '?':
			Fail("Nothing to repeat");
			return -1;
		case '{': {
			int a, b;
			size_t len;
			if (QuantBraces(a, b, len)) { Fail("Nothing to repeat"); return -1; }
			i_++;
			return AddChar('{');   // Annex B: a brace that is not a quantifier
		}
		case '(': return ParseGroup();
		case '[': return ParseClass();
		case '\\': return ParseEscape(assertion);
		default:
			i_++;
			return AddChar(c);   // includes a lone ']' and '}' (Annex B)
		}
	}

	int ParseGroup()
	{
		i_++;   // '('
		Node::Kind look = Node::Empty;
		if (Peek() == '?') {
			char32_t k = Peek(1);
			if (k == ':') i_ += 2;
			else if (k == '=') { look = Node::Look; i_ += 2; }
			else if (k == '!') { look = Node::NegLook; i_ += 2; }
			else if (k == '<' && (Peek(2) == '=' || Peek(2) == '!')) {
				Fail("lookbehind is not supported");
				return -1;
			} else if (k == '<') {
				// (?<name>...) -- a capture group; we keep no captures.
				size_t j = i_ + 2;
				while (j < p_.size() && p_[j] != '>' && (IsWord(p_[j]) || p_[j] == '$')) j++;
				if (j >= p_.size() || p_[j] != '>' || j == i_ + 2) {
					Fail("Invalid capture group name");
					return -1;
				}
				i_ = j + 1;
				rx_.groups++;
			} else {
				Fail("Invalid group");
				return -1;
			}
		} else {
			rx_.groups++;
		}
		int inner = ParseAlt();
		if (!err_.empty()) return -1;
		if (AtEnd() || Peek() != ')') {
			Fail("Unterminated group");
			return -1;
		}
		i_++;
		if (look == Node::Empty) return inner;
		Node n;
		n.kind = look;
		n.kids.push_back(inner);
		return Add(std::move(n));
	}

	// One escape after a backslash. Returns true with either *ch (a single
	// character) or *set (a class escape) filled. `inClass`: \b is backspace.
	bool Escape(bool inClass, char32_t& ch, std::vector<Range>& set, bool& isSet,
	            bool& isWordB, bool& isNotWordB)
	{
		isSet = isWordB = isNotWordB = false;
		i_++;   // '\'
		if (AtEnd()) { Fail("\\ at end of pattern"); return false; }
		char32_t c = Peek();
		i_++;
		switch (c) {
		case 'd': set = DigitRanges(); isSet = true; return true;
		case 'D': set = Complement(DigitRanges()); isSet = true; return true;
		case 's': set = SpaceRanges(); isSet = true; return true;
		case 'S': set = Complement(SpaceRanges()); isSet = true; return true;
		case 'w': set = WordRanges(); isSet = true; return true;
		case 'W': set = Complement(WordRanges()); isSet = true; return true;
		case 'b':
			if (inClass) { ch = 0x08; return true; }
			isWordB = true;
			return true;
		case 'B':
			if (inClass) { ch = 'B'; return true; }
			isNotWordB = true;
			return true;
		case 't': ch = 0x09; return true;
		case 'n': ch = 0x0A; return true;
		case 'v': ch = 0x0B; return true;
		case 'f': ch = 0x0C; return true;
		case 'r': ch = 0x0D; return true;
		case '0':
			if (IsDigit(Peek())) { Fail("octal escapes are not supported"); return false; }
			ch = 0;
			return true;
		case 'x': {
			int h1 = HexVal(Peek()), h2 = HexVal(Peek(1));
			if (h1 >= 0 && h2 >= 0) { ch = (char32_t)(h1 * 16 + h2); i_ += 2; }
			else ch = 'x';   // Annex B identity escape
			return true;
		}
		case 'u': {
			int v = 0;
			bool ok = true;
			for (int k = 0; k < 4; k++) {
				int h = HexVal(Peek(k));
				if (h < 0) { ok = false; break; }
				v = v * 16 + h;
			}
			if (ok) { ch = (char32_t)v; i_ += 4; }
			else ch = 'u';
			return true;
		}
		case 'c': {
			char32_t n = Peek();
			if (IsAsciiLetter(n) || (inClass && (IsDigit(n) || n == '_'))) {
				ch = n % 32;
				i_++;
			} else {
				// Annex B: "\c" not followed by a letter is a literal backslash,
				// and the 'c' is read again as an ordinary character.
				ch = '\\';
				i_--;
			}
			return true;
		}
		default:
			if (c >= '1' && c <= '9') {
				Fail("backreferences are not supported");
				return false;
			}
			ch = c;   // identity escape: \+ \) \. \- \\ \/ ...
			return true;
		}
	}

	int ParseEscape(bool& assertion)
	{
		char32_t ch = 0;
		std::vector<Range> set;
		bool isSet, wb, nwb;
		if (!Escape(false, ch, set, isSet, wb, nwb)) return -1;
		if (wb) { assertion = true; return AddKind(Node::WordB); }
		if (nwb) { assertion = true; return AddKind(Node::NotWordB); }
		if (isSet) return AddClass(std::move(set), false);
		return AddChar(ch);
	}

	// One class atom; *isSet when it was \d and friends.
	bool ClassAtom(char32_t& ch, std::vector<Range>& set, bool& isSet)
	{
		isSet = false;
		if (Peek() == '\\') {
			bool wb, nwb;
			return Escape(true, ch, set, isSet, wb, nwb);
		}
		ch = Peek();
		i_++;
		return true;
	}

	int ParseClass()
	{
		i_++;   // '['
		bool negated = false;
		if (Peek() == '^' && i_ < p_.size()) { negated = true; i_++; }
		std::vector<Range> ranges;
		for (;;) {
			if (AtEnd()) { Fail("Unterminated character class"); return -1; }
			if (Peek() == ']') { i_++; break; }
			char32_t a = 0;
			std::vector<Range> sa;
			bool aSet = false;
			if (!ClassAtom(a, sa, aSet)) return -1;
			if (Peek() == '-' && i_ + 1 < p_.size() && Peek(1) != ']') {
				i_++;   // '-'
				char32_t b = 0;
				std::vector<Range> sb;
				bool bSet = false;
				if (!ClassAtom(b, sb, bSet)) return -1;
				if (aSet || bSet) {
					// Annex B: a range with a class escape at either end is the
					// union of both ends and a literal '-'.
					if (aSet) ranges.insert(ranges.end(), sa.begin(), sa.end());
					else ranges.push_back({a, a});
					if (bSet) ranges.insert(ranges.end(), sb.begin(), sb.end());
					else ranges.push_back({b, b});
					ranges.push_back({'-', '-'});
					continue;
				}
				if (a > b) { Fail("Range out of order in character class"); return -1; }
				ranges.push_back({a, b});
				continue;
			}
			if (aSet) ranges.insert(ranges.end(), sa.begin(), sa.end());
			else ranges.push_back({a, a});
		}
		return AddClass(std::move(ranges), negated);
	}
};

bool AnchoredStart(const Rx& rx, int n)
{
	const Node& d = rx.nodes[n];
	switch (d.kind) {
	case Node::Begin: return true;
	case Node::Seq: return !d.kids.empty() && AnchoredStart(rx, d.kids[0]);
	case Node::Alt:
		for (int k : d.kids) if (!AnchoredStart(rx, k)) return false;
		return !d.kids.empty();
	case Node::Repeat: return d.min >= 1 && AnchoredStart(rx, d.kids[0]);
	default: return false;
	}
}

// ---- matcher ----------------------------------------------------------------

struct Cont {
	enum Kind : uint8_t { SeqRest, RepAfter } kind;
	int node;
	int count;       // SeqRest: next child index; RepAfter: iterations done
	size_t start;    // RepAfter: where this iteration began
	const Cont* next;
};

class Matcher {
public:
	Matcher(const Rx& rx, const std::u32string& t, const RxLimits& lim)
		: rx_(rx), t_(t), lim_(lim) {}

	RxStatus Search()
	{
		const size_t last = rx_.anchoredStart ? 0 : t_.size();
		for (size_t s = 0; s <= last; s++) {
			if (M(rx_.root, s, nullptr)) return RxStatus::Match;
			if (aborted_) return RxStatus::Aborted;
		}
		return RxStatus::NoMatch;
	}
	uint64_t steps = 0;

private:
	const Rx& rx_;
	const std::u32string& t_;
	const RxLimits& lim_;
	bool aborted_ = false;
	int depth_ = 0;

	bool Tick()
	{
		if (++steps > lim_.stepLimit) aborted_ = true;
		return !aborted_;
	}

	// Does the single-character node n accept the character at pos?
	bool One(const Node& n, size_t pos) const
	{
		if (pos >= t_.size()) return false;
		char32_t c = t_[pos];
		switch (n.kind) {
		case Node::Char: return (rx_.flags.icase ? Canon(c) : c) == n.ch;
		case Node::Any: return rx_.flags.dotAll || !IsLineTerm(c);
		case Node::Class: {
			const std::vector<Range>& r = rx_.classes[n.cls];
			bool in = InRanges(r, c);
			if (!in && rx_.flags.icase) {
				// ES CharacterSetMatcher: some a in the set with Canon(a) == Canon(c).
				const char32_t u = Canon(c);
				if (u != c && Canon(u) == u) in = InRanges(r, u);
				if (!in && c <= 0xFFFF) {
					const auto& inv = Cases().inv;
					auto it = std::lower_bound(inv.begin(), inv.end(), std::make_pair((char16_t)u, (char16_t)0));
					for (; !in && it != inv.end() && it->first == (char16_t)u; ++it)
						in = it->second != c && InRanges(r, it->second);
				}
			}
			return in != n.negated;
		}
		default: return false;
		}
	}

	static bool IsSingle(const Node& n)
	{
		return n.kind == Node::Char || n.kind == Node::Any || n.kind == Node::Class;
	}

	bool WordAt(size_t pos) const { return pos < t_.size() && IsWord(t_[pos]); }

	bool Next(const Cont* k, size_t pos)
	{
		if (!k) return true;
		if (k->kind == Cont::SeqRest) {
			const Node& s = rx_.nodes[k->node];
			if (k->count >= (int)s.kids.size()) return Next(k->next, pos);
			Cont c{Cont::SeqRest, k->node, k->count + 1, 0, k->next};
			return M(s.kids[k->count], pos, &c);
		}
		// RepAfter: ES RepeatMatcher -- an iteration taken when the minimum was
		// already met must consume something, or it fails.
		const Node& r = rx_.nodes[k->node];
		if (pos == k->start && k->count - 1 >= r.min) return false;
		return RepStep(k->node, k->count, pos, k->next);
	}

	bool RepStep(int node, int count, size_t pos, const Cont* k)
	{
		const Node& r = rx_.nodes[node];
		if (r.max >= 0 && count >= r.max) return Next(k, pos);
		Cont c{Cont::RepAfter, node, count + 1, pos, k};
		if (count < r.min) return M(r.kids[0], pos, &c);
		if (r.greedy) {
			if (M(r.kids[0], pos, &c)) return true;
			if (aborted_) return false;
			return Next(k, pos);
		}
		if (Next(k, pos)) return true;
		if (aborted_) return false;
		return M(r.kids[0], pos, &c);
	}

	struct DepthGuard {
		int& d;
		explicit DepthGuard(int& x) : d(x) { d++; }
		~DepthGuard() { d--; }
	};

	bool M(int ni, size_t pos, const Cont* k)
	{
		if (aborted_ || !Tick()) return false;
		DepthGuard g(depth_);
		if (depth_ > lim_.depthLimit) { aborted_ = true; return false; }
		const Node& n = rx_.nodes[ni];
		switch (n.kind) {
		case Node::Empty: return Next(k, pos);
		case Node::Char:
		case Node::Any:
		case Node::Class: return One(n, pos) && Next(k, pos + 1);
		case Node::Begin: return pos == 0 && Next(k, pos);
		case Node::End: return pos == t_.size() && Next(k, pos);
		case Node::WordB:
		case Node::NotWordB: {
			bool a = pos > 0 && WordAt(pos - 1);
			bool b = WordAt(pos);
			return ((a != b) == (n.kind == Node::WordB)) && Next(k, pos);
		}
		case Node::Seq: {
			if (n.kids.empty()) return Next(k, pos);
			Cont c{Cont::SeqRest, ni, 1, 0, k};
			return M(n.kids[0], pos, &c);
		}
		case Node::Alt:
			for (int kid : n.kids) {
				if (M(kid, pos, k)) return true;
				if (aborted_) return false;
			}
			return false;
		case Node::Repeat: {
			const Node& kid = rx_.nodes[n.kids[0]];
			if (IsSingle(kid)) {
				// Fast path for x*, [0-9]{2,}, .*? ...: no recursion per character.
				size_t avail = 0;
				const size_t cap = n.max < 0 ? (size_t)-1 : (size_t)n.max;
				while (avail < cap && One(kid, pos + avail)) {
					avail++;
					if (!Tick()) return false;
				}
				if (avail < (size_t)n.min) return false;
				if (n.greedy) {
					for (size_t j = avail + 1; j-- > (size_t)n.min;) {
						if (Next(k, pos + j)) return true;
						if (aborted_) return false;
					}
				} else {
					for (size_t j = (size_t)n.min; j <= avail; j++) {
						if (Next(k, pos + j)) return true;
						if (aborted_) return false;
					}
				}
				return false;
			}
			return RepStep(ni, 0, pos, k);
		}
		case Node::Look:
		case Node::NegLook: {
			bool hit = M(n.kids[0], pos, nullptr);
			if (aborted_) return false;
			if (hit != (n.kind == Node::Look)) return false;
			return Next(k, pos);
		}
		}
		return false;
	}
};

} // namespace

char32_t RxCanonicalize(char32_t c) { return Canon(c); }

bool RxDecodeUtf8(std::string_view s, std::u32string& out)
{
	out.clear();
	out.reserve(s.size());
	bool ok = true;
	size_t i = 0;
	const size_t n = s.size();
	while (i < n) {
		unsigned char b = (unsigned char)s[i];
		if (b < 0x80) { out.push_back(b); i++; continue; }
		int len = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
		char32_t cp = len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
		bool good = len > 0 && i + len <= n;
		for (int k = 1; good && k < len; k++) {
			unsigned char c = (unsigned char)s[i + k];
			if ((c & 0xC0) != 0x80) good = false;
			else cp = (cp << 6) | (c & 0x3F);
		}
		if (good) {
			static const char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
			if (cp < kMin[len] || cp > kMaxCp || (cp >= 0xD800 && cp <= 0xDFFF)) good = false;
		}
		if (!good) { out.push_back(0xFFFD); ok = false; i++; continue; }
		out.push_back(cp);
		i += len;
	}
	return ok;
}

std::optional<Rx> RxCompile(std::string_view pattern, std::string* err, RxFlags flags)
{
	std::u32string p;
	if (!RxDecodeUtf8(pattern, p)) {
		if (err) *err = "pattern is not valid UTF-8";
		return std::nullopt;
	}
	Rx rx;
	rx.flags = flags;
	Parser parser(p, rx);
	if (!parser.Run(err)) return std::nullopt;
	rx.anchoredStart = AnchoredStart(rx, rx.root);
	if (err) err->clear();
	return rx;
}

RxStatus RxSearchCps(const Rx& rx, const std::u32string& text, const RxLimits& lim,
                     uint64_t* steps)
{
	Matcher m(rx, text, lim);
	RxStatus st = m.Search();
	if (steps) *steps = m.steps;
	return st;
}

RxStatus RxSearchEx(const Rx& rx, std::string_view text, const RxLimits& lim, uint64_t* steps)
{
	std::u32string t;
	RxDecodeUtf8(text, t);
	return RxSearchCps(rx, t, lim, steps);
}

bool RxSearch(const Rx& rx, std::string_view text)
{
	return RxSearchEx(rx, text) == RxStatus::Match;
}
